"""Coordinated version metadata and fail-closed release publication."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
from urllib.request import Request, urlopen


VERSION = Path("VERSION")
CMAKE = Path("facturx/CMakeLists.txt")
PLIST = Path("facturx/Info.plist.in")
SEMVER = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
SETTINGS = {
    "CFBundleShortVersionString": "MACOSX_BUNDLE_SHORT_VERSION_STRING",
    "CFBundleVersion": "MACOSX_BUNDLE_BUNDLE_VERSION",
}


def require_version(value):
    if not isinstance(value, str) or not re.fullmatch(SEMVER, value, re.ASCII):
        raise ValueError(f"Invalid semantic version: {value!r}; expected X.Y.Z without leading zeros")
    return value


def validate_plist(data, version, allow_variables=True):
    for key, setting in SETTINGS.items():
        value = data.get(key)
        if allow_variables and value == f"${{{setting}}}":
            continue
        if require_version(value) != version:
            raise ValueError(f"{key} drift: {value!r} != {version}")
    if data.get("CFBundlePackageType") != "BNDL":
        raise ValueError("The plugin must have CFBundlePackageType BNDL")


def read_version():
    raw = VERSION.read_text()
    if not re.fullmatch(SEMVER + r"\n?", raw, re.ASCII):
        raise ValueError("VERSION must contain only X.Y.Z and an optional final newline")
    version = require_version(raw.removesuffix("\n"))
    cmake = CMAKE.read_text()
    for setting in SETTINGS.values():
        values = re.findall(rf'^\s*{setting}\s+"([^"]+)"', cmake, re.M)
        if len(values) != 1:
            raise ValueError(f"Expected exactly one quoted {setting} in CMake")
        if values[0] != "${FACTURX_VERSION}" and require_version(values[0]) != version:
            raise ValueError(f"{setting} drift: {values[0]!r} != {version}")
    with PLIST.open("rb") as source:
        validate_plist(plistlib.load(source), version)
    return version


def bump(mode):
    current = read_version()
    parts = list(map(int, current.split(".")))
    index = ("major", "minor", "patch").index(mode)
    parts[index] += 1
    parts[index + 1:] = [0] * (2 - index)
    new = ".".join(map(str, parts))
    cmake = CMAKE.read_text()
    for setting in SETTINGS.values():
        cmake = re.sub(
            rf'(^\s*{setting}\s+"){re.escape(current)}(")',
            rf"\g<1>{new}\2", cmake, flags=re.M,
        )
    plist = PLIST.read_text()
    for key in SETTINGS:
        plist = re.sub(
            rf"(<key>{key}</key>\s*<string>){re.escape(current)}(</string>)",
            rf"\g<1>{new}\2", plist,
        )
    CMAKE.write_text(cmake)
    PLIST.write_text(plist)
    VERSION.write_text(new + "\n")
    if read_version() != new:
        raise ValueError("Bumped metadata did not validate")
    return new


def check_commit():
    sha = os.environ["GITHUB_SHA"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError(f"Invalid selected commit: {sha!r}")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if actual != sha:
        raise ValueError(f"Checkout {actual} does not match selected commit {sha}")
    return sha


def release_api(path, payload=None):
    request = Request(
        f"{os.environ['GITHUB_API_URL']}/repos/{os.environ['GITHUB_REPOSITORY']}/{path}",
        data=None if payload is None else json.dumps(payload).encode("utf-8"),
        method="GET" if payload is None else "POST",
        headers={
            "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urlopen(request, timeout=30) as response:
        return json.load(response)


def ensure_available(tag):
    result = subprocess.run(
        ["git", "ls-remote", "--exit-code", "--tags", "origin", f"refs/tags/{tag}"],
        text=True, capture_output=True, check=False,
    )
    if result.returncode == 0:
        raise ValueError(f"Tag {tag} already exists; bump the version before building")
    if result.returncode != 2:
        raise RuntimeError(f"Cannot check remote tags: {result.stderr}")
    # Listing with authentication includes drafts, unlike the tag lookup.
    page = 1
    while True:
        releases = release_api(f"releases?per_page=100&page={page}")
        if not isinstance(releases, list) or len(releases) > 100:
            raise ValueError("Invalid releases listing response")
        for item in releases:
            if (not isinstance(item, dict) or not isinstance(item.get("tag_name"), str)
                    or not isinstance(item.get("draft"), bool)):
                raise ValueError("Invalid release in listing response")
            if item["tag_name"] == tag:
                raise ValueError(f"Release {tag} already exists (including drafts); refusing to replace it or its assets")
        if len(releases) < 100:
            return
        page += 1


def publish(version, assets):
    sha = check_commit()
    if read_version() != require_version(version):
        raise ValueError("Publication version differs from selected commit metadata")
    for asset in assets:
        if not Path(asset).is_file() or Path(asset).stat().st_size == 0:
            raise ValueError(f"Missing or empty release asset: {asset}")
    tag = f"v{version}"
    ensure_available(tag)
    subprocess.run(["git", "tag", tag, sha], check=True)
    subprocess.run([
        "git", "-c", "push.followTags=false", "push", "origin",
        f"refs/tags/{tag}:refs/tags/{tag}",
    ], check=True)
    remote = subprocess.check_output(
        ["git", "ls-remote", "--exit-code", "--tags", "origin", f"refs/tags/{tag}"], text=True,
    ).split()
    if remote != [sha, f"refs/tags/{tag}"]:
        raise ValueError("Published tag does not point to the selected commit")
    created = release_api("releases", {
        "tag_name": tag,
        "target_commitish": sha,
        "name": tag,
        "draft": False,
        "prerelease": False,
        "generate_release_notes": True,
    })
    if (not isinstance(created, dict) or type(created.get("id")) is not int
            or created["id"] <= 0 or created.get("tag_name") != tag
            or created.get("draft") is not False or created.get("prerelease") is not False):
        raise ValueError("Unexpected release creation response; inspect publication state manually")
    # Separate upload avoids gh release create's automatic temporary-draft deletion.
    subprocess.run(["gh", "release", "upload", tag, *assets], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("version")
    commands.add_parser("preflight")
    commands.add_parser("check-commit")
    bump_parser = commands.add_parser("bump")
    bump_parser.add_argument("mode", choices=("patch", "minor", "major"))
    verify = commands.add_parser("verify-bundle")
    verify.add_argument("plist", type=Path)
    release = commands.add_parser("publish")
    release.add_argument("version")
    release.add_argument("assets", nargs=2)
    args = parser.parse_args()
    if args.command == "check-commit":
        check_commit()
        return
    if args.command == "bump":
        version = bump(args.mode)
    elif args.command == "publish":
        publish(args.version, args.assets)
        return
    else:
        version = read_version()
        if args.command == "preflight":
            check_commit()
            ensure_available(f"v{version}")
        elif args.command == "verify-bundle":
            with args.plist.open("rb") as source:
                validate_plist(plistlib.load(source), version, allow_variables=False)
    print(version)
    if output := os.environ.get("GITHUB_OUTPUT"):
        with open(output, "a") as destination:
            destination.write(f"version={version}\ntag=v{version}\n")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}", file=sys.stderr)
        sys.exit(1)
