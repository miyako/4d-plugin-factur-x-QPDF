import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

import release


ROOT = Path(__file__).resolve().parents[2]
SHA = "a" * 40
FIXTURE_VERSION = "2.3.4"


class Fixture(unittest.TestCase):
    def setUp(self):
        self.previous = Path.cwd()
        self.temp = tempfile.TemporaryDirectory()
        os.chdir(self.temp.name)
        Path("facturx").mkdir()
        Path("VERSION").write_text(FIXTURE_VERSION + "\n")
        Path("facturx/CMakeLists.txt").write_text(
            'include("${CMAKE_CURRENT_SOURCE_DIR}/Version.cmake")\n'
            'MACOSX_BUNDLE_BUNDLE_VERSION "${FACTURX_VERSION}"\n'
            'MACOSX_BUNDLE_SHORT_VERSION_STRING "${FACTURX_VERSION}"\n'
        )
        self.plist = {
            "CFBundlePackageType": "BNDL",
            "CFBundleVersion": "${MACOSX_BUNDLE_BUNDLE_VERSION}",
            "CFBundleShortVersionString": "${MACOSX_BUNDLE_SHORT_VERSION_STRING}",
        }
        self.write_plist()
        self.env = patch.dict(os.environ, {
            "GITHUB_SHA": SHA,
            "GITHUB_API_URL": "https://api.github.test",
            "GITHUB_REPOSITORY": "owner/repo",
            "GH_TOKEN": "fixture-token",
        })
        self.env.start()

    def tearDown(self):
        self.env.stop()
        os.chdir(self.previous)
        self.temp.cleanup()

    def write_plist(self):
        Path("facturx/Info.plist.in").write_bytes(plistlib.dumps(self.plist))


class VersionTests(Fixture):
    def test_reference_resolution_and_bump_modes(self):
        for mode, expected in (("patch", "2.3.5"), ("minor", "2.4.0"), ("major", "3.0.0")):
            with self.subTest(mode=mode):
                Path("VERSION").write_text(FIXTURE_VERSION + "\n")
                cmake = release.CMAKE.read_bytes()
                plist = release.PLIST.read_bytes()
                self.assertEqual(release.bump(mode), expected)
                self.assertEqual(release.read_version(), expected)
                self.assertEqual(release.CMAKE.read_bytes(), cmake)
                self.assertEqual(release.PLIST.read_bytes(), plist)

    def test_literal_metadata_is_coordinated(self):
        release.CMAKE.write_text(release.CMAKE.read_text().replace("${FACTURX_VERSION}", FIXTURE_VERSION))
        self.plist.update({key: FIXTURE_VERSION for key in release.SETTINGS})
        self.write_plist()
        self.assertEqual(release.bump("patch"), "2.3.5")
        self.assertEqual(release.read_version(), "2.3.5")
        self.assertNotIn(FIXTURE_VERSION, release.CMAKE.read_text())
        self.assertNotIn(FIXTURE_VERSION, release.PLIST.read_text())

    def test_invalid_versions_fail_without_writes(self):
        for value in ("", "1.2", "v1.2.3", "01.2.3", "1.2.03", "1.2.3-beta", "1.2.3\n\n",
                      "1.2.3;exit", " 1.2.3", "1.2.3\njunk", "１.2.3"):
            with self.subTest(value=value):
                release.VERSION.write_text(value)
                before = release.CMAKE.read_bytes(), release.PLIST.read_bytes()
                with self.assertRaises(ValueError):
                    release.bump("patch")
                self.assertEqual(before, (release.CMAKE.read_bytes(), release.PLIST.read_bytes()))

    def test_cmake_drift_invalid_missing_duplicate(self):
        original = release.CMAKE.read_text()
        for replacement in ('"2.3.3"', '"bad"', '"${UNKNOWN}"'):
            release.CMAKE.write_text(original.replace('"${FACTURX_VERSION}"', replacement, 1))
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                release.read_version()
        for text in ("", original + 'MACOSX_BUNDLE_BUNDLE_VERSION "2.3.4"\n'):
            release.CMAKE.write_text(text)
            with self.assertRaises(ValueError):
                release.read_version()

    def test_plist_drift_invalid_missing_and_wrong_references(self):
        original = dict(self.plist)
        for key in release.SETTINGS:
            for value in ("2.3.3", "bad", "${UNKNOWN}", 234, None):
                self.plist = dict(original)
                if value is None:
                    del self.plist[key]
                else:
                    self.plist[key] = value
                self.write_plist()
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    release.read_version()

    def test_built_plist_rejects_references_drift_and_wrong_package(self):
        with self.assertRaises(ValueError):
            release.validate_plist(self.plist, FIXTURE_VERSION, allow_variables=False)
        built = {key: FIXTURE_VERSION for key in release.SETTINGS}
        built["CFBundlePackageType"] = "BNDL"
        release.validate_plist(built, FIXTURE_VERSION, allow_variables=False)
        for key, value in (("CFBundleVersion", "2.3.3"), ("CFBundlePackageType", "APPL")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                release.validate_plist({**built, key: value}, FIXTURE_VERSION, allow_variables=False)

    def test_cli_output(self):
        output = Path("output")
        with patch.dict(os.environ, GITHUB_OUTPUT=str(output)), \
                patch.object(sys, "argv", ["release.py", "version"]), \
                contextlib.redirect_stdout(io.StringIO()) as stdout:
            release.main()
        self.assertEqual(stdout.getvalue(), "2.3.4\n")
        self.assertEqual(output.read_text(), "version=2.3.4\ntag=v2.3.4\n")

    def test_cmake_loader_validates_and_resolves(self):
        shutil.copyfile(ROOT / "facturx/Version.cmake", "facturx/Version.cmake")
        Path("probe.cmake").write_text(
            'include("${CMAKE_CURRENT_LIST_DIR}/facturx/Version.cmake")\n'
            'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/resolved" "${FACTURX_VERSION}")\n'
            'set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${FACTURX_VERSION}")\n'
            'set(MACOSX_BUNDLE_BUNDLE_VERSION "${FACTURX_VERSION}")\n'
            'configure_file(facturx/Info.plist.in built.plist)\n'
        )
        for value in ("2.3.4", "2.3.4\n", "12.34.56\n"):
            release.VERSION.write_text(value)
            subprocess.run(["cmake", "-P", "probe.cmake"], check=True, capture_output=True)
            self.assertEqual(Path("resolved").read_text(), value.rstrip("\n"))
            release.validate_plist(
                plistlib.loads(Path("built.plist").read_bytes()),
                value.rstrip("\n"), allow_variables=False,
            )
        for value in ("", "1.2", "01.2.3", "1.2.03", "1.2.3-beta", "1.2.3\n\n", " 1.2.3"):
            release.VERSION.write_text(value)
            result = subprocess.run(["cmake", "-P", "probe.cmake"], capture_output=True)
            self.assertNotEqual(result.returncode, 0, value)


class AvailabilityTests(Fixture):
    def check_remote(self, code=2, error=None, pages=None):
        result = subprocess.CompletedProcess([], code, "", "remote failed")
        responses = [io.BytesIO(json.dumps(page).encode()) for page in (pages or [[]])]
        return patch.object(release.subprocess, "run", return_value=result), \
            patch.object(release, "urlopen", side_effect=error or responses)

    def test_successful_authenticated_empty_listing_is_absence(self):
        run, urlopen = self.check_remote()
        with run as git, urlopen as api:
            release.ensure_available("v2.3.4")
        self.assertEqual(git.call_args.args[0][-1], "refs/tags/v2.3.4")
        request = api.call_args.args[0]
        self.assertTrue(request.full_url.endswith("/repos/owner/repo/releases?per_page=100&page=1"))
        self.assertEqual(request.get_header("Authorization"), "Bearer fixture-token")
        self.assertEqual(request.get_method(), "GET")
        self.assertEqual(api.call_args.kwargs["timeout"], 30)

    def test_pagination_checks_later_drafts_and_published_releases(self):
        first = [{"tag_name": f"v0.0.{i}", "draft": False} for i in range(100)]
        for draft in (True, False):
            run, urlopen = self.check_remote(pages=[first, [{"tag_name": "v2.3.4", "draft": draft}]])
            with self.subTest(draft=draft), run, urlopen as api, \
                    self.assertRaisesRegex(ValueError, "refusing to replace"):
                release.ensure_available("v2.3.4")
            self.assertEqual(api.call_count, 2)
            self.assertTrue(api.call_args.args[0].full_url.endswith("page=2"))

    def test_full_page_requires_successful_next_page_before_absence(self):
        first = [{"tag_name": f"v0.0.{i}", "draft": True} for i in range(100)]
        run, urlopen = self.check_remote(pages=[first, []])
        with run, urlopen as api:
            release.ensure_available("v2.3.4")
        self.assertEqual(api.call_count, 2)
        run, urlopen = self.check_remote()
        with run, urlopen as api:
            api.side_effect = [
                io.BytesIO(json.dumps(first).encode()),
                HTTPError("https://api.github.test", 500, "failure", {}, None),
            ]
            with self.assertRaises(HTTPError):
                release.ensure_available("v2.3.4")

    def test_existing_tag_blocks_without_api(self):
        run, urlopen = self.check_remote(code=0)
        with run, urlopen as api, self.assertRaisesRegex(ValueError, "already exists"):
            release.ensure_available("v2.3.4")
        api.assert_not_called()

    def test_remote_failure_is_not_absence(self):
        for code in (1, 128):
            run, urlopen = self.check_remote(code=code)
            with run, urlopen as api, self.assertRaisesRegex(RuntimeError, "Cannot check"):
                release.ensure_available("v2.3.4")
            api.assert_not_called()

    def test_existing_release_blocks(self):
        for draft in (True, False):
            run, urlopen = self.check_remote(pages=[[{"tag_name": "v2.3.4", "draft": draft}]])
            with self.subTest(draft=draft), run, urlopen, \
                    self.assertRaisesRegex(ValueError, "refusing to replace"):
                release.ensure_available("v2.3.4")

    def test_api_errors_are_not_absence(self):
        errors = [HTTPError("https://api.github.test", code, "failure", {}, None)
                  for code in (401, 403, 404, 429, 500)]
        errors += [URLError("offline"), TimeoutError("timeout")]
        for error in errors:
            run, urlopen = self.check_remote(error=error)
            with self.subTest(error=error), run, urlopen, self.assertRaises(type(error)):
                release.ensure_available("v2.3.4")

    def test_malformed_listing_fails_closed(self):
        for page in ({}, None, [None], [{"tag_name": "v1.2.3"}],
                     [{"tag_name": 123, "draft": False}], [{"tag_name": "v1.2.3", "draft": "false"}]):
            run, urlopen = self.check_remote(pages=[page])
            with self.subTest(page=page), run, urlopen, self.assertRaises(ValueError):
                release.ensure_available("v2.3.4")
        run, urlopen = self.check_remote()
        with run, urlopen as api:
            api.side_effect = [io.BytesIO(b"invalid JSON")]
            with self.assertRaises(ValueError):
                release.ensure_available("v2.3.4")

    def test_create_api_is_json_post(self):
        payload = {
            "tag_name": "v2.3.4", "target_commitish": SHA, "name": "v2.3.4",
            "draft": False, "prerelease": False, "generate_release_notes": True,
        }
        with patch.object(release, "urlopen", return_value=io.BytesIO(b'{"id": 123}')) as api:
            self.assertEqual(release.release_api("releases", payload), {"id": 123})
        request = api.call_args.args[0]
        self.assertTrue(request.full_url.endswith("/repos/owner/repo/releases"))
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(json.loads(request.data), payload)
        self.assertEqual(request.get_header("Authorization"), "Bearer fixture-token")
        self.assertEqual(request.get_header("Content-type"), "application/json")


class PublicationTests(Fixture):
    def setUp(self):
        super().setUp()
        self.assets = ["facturx-2.3.4.zip", "facturx-2.3.4.dmg"]
        self.created = {"id": 123, "tag_name": "v2.3.4", "draft": False, "prerelease": False}
        for asset in self.assets:
            Path(asset).write_bytes(b"fixture")

    def test_immutable_selected_commit(self):
        with patch.object(release.subprocess, "check_output", return_value=SHA + "\n"):
            self.assertEqual(release.check_commit(), SHA)
        with patch.object(release.subprocess, "check_output", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "does not match"):
                release.check_commit()
        for value in ("main", "", "a" * 39, "A" * 40):
            with patch.dict(os.environ, GITHUB_SHA=value), self.assertRaises(ValueError):
                release.check_commit()

    def test_exact_tag_target_and_publication_order(self):
        calls = []

        def create(path, payload):
            calls.append(["POST", path, payload])
            return self.created

        with patch.object(release, "check_commit", return_value=SHA), \
                patch.object(release, "ensure_available", side_effect=lambda tag: calls.append(["available", tag])), \
                patch.object(release.subprocess, "run", side_effect=lambda args, **kw: calls.append(args)), \
                patch.object(release, "release_api", side_effect=create), \
                patch.object(release.subprocess, "check_output", return_value=f"{SHA}\trefs/tags/v2.3.4\n"):
            release.publish(FIXTURE_VERSION, self.assets)
        self.assertEqual(calls, [
            ["available", "v2.3.4"],
            ["git", "tag", "v2.3.4", SHA],
            ["git", "-c", "push.followTags=false", "push", "origin", "refs/tags/v2.3.4:refs/tags/v2.3.4"],
            ["POST", "releases", {
                "tag_name": "v2.3.4", "target_commitish": SHA, "name": "v2.3.4",
                "draft": False, "prerelease": False, "generate_release_notes": True,
            }],
            ["gh", "release", "upload", "v2.3.4", *self.assets],
        ])

    def test_no_publication_for_wrong_version_or_assets(self):
        with patch.object(release, "check_commit", return_value=SHA), \
                patch.object(release, "ensure_available") as available, \
                patch.object(release.subprocess, "run") as run:
            with self.assertRaises(ValueError):
                release.publish("2.3.5", self.assets)
            for content in (None, b""):
                Path(self.assets[0]).unlink(missing_ok=True)
                if content is not None:
                    Path(self.assets[0]).write_bytes(content)
                with self.assertRaises(ValueError):
                    release.publish(FIXTURE_VERSION, self.assets)
        available.assert_not_called()
        run.assert_not_called()

    def test_collision_at_final_check_stops_tagging(self):
        with patch.object(release, "check_commit", return_value=SHA), \
                patch.object(release, "ensure_available", side_effect=ValueError("collision")), \
                patch.object(release.subprocess, "run") as run, self.assertRaises(ValueError):
            release.publish(FIXTURE_VERSION, self.assets)
        run.assert_not_called()

    def test_failure_after_tag_push_never_cleans_up_or_overwrites(self):
        for failed_call in (1, 2, 3):
            calls = []

            def execute(args, **kwargs):
                calls.append(args)
                if len(calls) == failed_call:
                    raise subprocess.CalledProcessError(1, args)

            with self.subTest(failed_call=failed_call), \
                    patch.object(release, "check_commit", return_value=SHA), \
                    patch.object(release, "ensure_available"), \
                    patch.object(release.subprocess, "run", side_effect=execute), \
                    patch.object(release, "release_api", return_value=self.created) as api, \
                    patch.object(release.subprocess, "check_output", return_value=f"{SHA}\trefs/tags/v2.3.4"), \
                    self.assertRaises(subprocess.CalledProcessError):
                release.publish(FIXTURE_VERSION, self.assets)
            self.assertEqual(len(calls), failed_call)
            self.assertFalse(any("--force" in c or "--clobber" in c or "delete" in c for c in calls))
            self.assertEqual(api.call_count, int(failed_call == 3))

    def test_post_failure_leaves_pushed_tag_without_upload_or_cleanup(self):
        for error in (
            HTTPError("https://api.github.test", 422, "duplicate", {}, None),
            HTTPError("https://api.github.test", 500, "failure", {}, None),
            URLError("response lost after server creation"),
        ):
            with self.subTest(error=error), \
                    patch.object(release, "check_commit", return_value=SHA), \
                    patch.object(release, "ensure_available"), \
                    patch.object(release.subprocess, "run") as run, \
                    patch.object(release.subprocess, "check_output", return_value=f"{SHA}\trefs/tags/v2.3.4"), \
                    patch.object(release, "release_api", side_effect=error) as api, \
                    self.assertRaises(type(error)):
                release.publish(FIXTURE_VERSION, self.assets)
            self.assertEqual(run.call_count, 2)
            self.assertTrue(all(call.args[0][0] == "git" for call in run.call_args_list))
            self.assertEqual(api.call_count, 1)
            self.assertEqual(api.call_args.args[0], "releases")

    def test_unexpected_post_response_stops_without_cleanup(self):
        for response in ({}, [], {**self.created, "draft": True},
                         {**self.created, "tag_name": "v2.3.5"}, {**self.created, "id": True}):
            with self.subTest(response=response), \
                    patch.object(release, "check_commit", return_value=SHA), \
                    patch.object(release, "ensure_available"), \
                    patch.object(release.subprocess, "run") as run, \
                    patch.object(release.subprocess, "check_output", return_value=f"{SHA}\trefs/tags/v2.3.4"), \
                    patch.object(release, "release_api", return_value=response) as api, \
                    self.assertRaisesRegex(ValueError, "inspect publication state"):
                release.publish(FIXTURE_VERSION, self.assets)
            self.assertEqual(run.call_count, 2)
            self.assertEqual(api.call_count, 1)

    def test_wrong_remote_tag_target_blocks_release(self):
        with patch.object(release, "check_commit", return_value=SHA), \
                patch.object(release, "ensure_available"), \
                patch.object(release.subprocess, "run") as run, \
                patch.object(release.subprocess, "check_output", return_value=f"{'b' * 40}\trefs/tags/v2.3.4"), \
                self.assertRaisesRegex(ValueError, "does not point"):
            release.publish(FIXTURE_VERSION, self.assets)
        self.assertEqual(run.call_count, 2)

    def test_nonforced_push_rejects_real_remote_collision(self):
        subprocess.run(["git", "init", "--bare", "remote.git"], check=True, capture_output=True)
        subprocess.run(["git", "init", "checkout"], check=True, capture_output=True)
        cwd = Path("checkout")

        def git(*args):
            return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True)

        git("config", "user.name", "Fixture")
        git("config", "user.email", "fixture@example.test")
        git("remote", "add", "origin", str(Path("remote.git").resolve()))
        git("commit", "--allow-empty", "-m", "first")
        git("tag", "v2.3.4")
        git("push", "origin", "refs/tags/v2.3.4:refs/tags/v2.3.4")
        git("tag", "-d", "v2.3.4")
        git("commit", "--allow-empty", "-m", "second")
        git("tag", "v2.3.4")
        with self.assertRaises(subprocess.CalledProcessError):
            git("-c", "push.followTags=false", "push", "origin", "refs/tags/v2.3.4:refs/tags/v2.3.4")


class RepositoryTests(unittest.TestCase):
    def test_live_metadata_matches_authoritative_version(self):
        previous = Path.cwd()
        try:
            os.chdir(ROOT)
            self.assertEqual(release.read_version(), Path("VERSION").read_text().rstrip("\n"))
        finally:
            os.chdir(previous)
        cmake = (ROOT / release.CMAKE).read_text()
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/Version.cmake")', cmake)

    def test_zero_input_manual_trigger_and_cross_branch_serialization(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        self.assertTrue(workflow.startswith("name: Build\n"))
        trigger = workflow.split("\non:\n", 1)[1].split("\npermissions:", 1)[0]
        self.assertEqual(trigger.strip(), "workflow_dispatch:")
        concurrency = workflow.split("\nconcurrency:\n", 1)[1].split("\njobs:", 1)[0]
        self.assertIn("group: release-publication", concurrency)
        self.assertNotIn("github.ref", concurrency)
        self.assertIn("cancel-in-progress: false", concurrency)

    def test_all_release_jobs_use_selected_sha_and_preflight(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        for name in ("version", "build-windows", "build-macos"):
            block = workflow.split(f"\n  {name}:\n", 1)[1].split("\n  build-", 1)[0]
            self.assertIn("ref: ${{ github.sha }}", block)
            self.assertIn("release.py preflight", block)
            if "cmake -S" in block:
                self.assertLess(block.index("release.py preflight"), block.index("cmake -S"))
        self.assertNotIn("inputs.tag", workflow)
        self.assertNotIn("needs.version.outputs.tag }}", workflow)

    def test_publication_requires_all_existing_build_and_signing_gates(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        mac = workflow.split("\n  build-macos:\n", 1)[1]
        self.assertIn("needs: [version, build-windows]", mac)
        self.assertIn("needs: version", workflow.split("\n  build-windows:\n", 1)[1])
        gates = [
            "release.py preflight", "actions/download-artifact@v4",
            "security import", "notarytool store-credentials",
            "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_ARCHITECTURES=x86_64",
            "lipo -create", "grep -q arm64", "grep -q x86_64",
            "cp winbuild/facturx.4DX", "release.py verify-bundle",
            "codesign --verbose --deep --timestamp --force --options=runtime",
            "ditto -c -k --keepParent", "hdiutil create", 'notarytool submit "$ZIP_PATH"',
            'notarytool submit "$DMG_PATH"', 'r.get("status") == "Accepted"',
            'stapler staple "$DMG_PATH"', 'stapler validate "$DMG_PATH"', "release.py publish",
        ]
        positions = [mac.index(gate) for gate in gates]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("continue-on-error", mac)
        self.assertNotIn("always()", mac)
        for secret in ("APPLE_DEVELOPER_ID_CERTIFICATE", "APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD",
                       "KEYCHAIN_PASSWORD", "NOTARYTOOL_APPLE_ID", "NOTARYTOOL_TEAM_ID", "NOTARYTOOL_PASSWORD"):
            self.assertIn("${{ secrets." + secret + " }}", mac)

    def test_bump_is_selected_branch_only_and_no_tags(self):
        workflow = (ROOT / ".github/workflows/bump-version.yml").read_text()
        for expected in ("- patch", "- minor", "- major", "group: bump-version-${{ github.ref }}",
                         "cancel-in-progress: false", "ref: ${{ github.sha }}",
                         "github.ref_type == 'branch'", 'if [ "$REF_TYPE" != branch ]',
                         "release.py check-commit", "release.py bump", "SELECTED_REF: ${{ github.ref }}",
                         'git -c push.followTags=false push origin "HEAD:${SELECTED_REF}"'):
            self.assertIn(expected, workflow)
        for forbidden in ("git tag", "refs/tags/", "--tags", "--force", "gh release", "workflow run"):
            self.assertNotIn(forbidden, workflow)

    def test_qpdf_test_requirements_remain(self):
        workflow = (ROOT / ".github/workflows/test.yml").read_text()
        for expected in ("- '**'", "pull_request:", "fail-fast: false", "windows-latest",
                         "macos-latest", "Verify exported symbols", "make-pdfa3.sh",
                         "--startup-method=test_all", "grep -q PASS", "Select-String",
                         "verapdf --flavour 3b"):
            self.assertIn(expected, workflow)


if __name__ == "__main__":
    unittest.main()
