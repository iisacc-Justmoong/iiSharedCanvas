#!/usr/bin/env python3
"""Exercise the real resource install rule without launching an application."""
import os
import errno
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, os.environ.get("IIFILEPROVIDER_PYTHON_DIRECTORY") or
                str(Path.home()/".local/SDK/iiFileProvider/share/iiFileProvider/python"))
import ii_file_provider as provider


def remove_owned(path):
    for attempt in range(3):
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except OSError as error:
            if error.errno != errno.ENOTEMPTY or attempt == 2:
                raise
            time.sleep(0.05)


class ResourceInstallPolicyTest(unittest.TestCase):
    def test_private_resource_state_is_not_staged(self):
        token = uuid.uuid4().hex
        resources = ROOT/"resources"
        workspace = resources/(".iisacc-resource-policy-" + token)
        build = ROOT/"build"/("resource-policy-" + token)
        if any(path.is_symlink() or path.exists() for path in (workspace, build)):
            self.fail("Refusing to replace an existing fixture")
        provider.mkdir(workspace, parents=True)
        try:
            source = workspace/"project"
            fixture = source/"fixtures"
            stage = workspace/"stage"
            for relative, contents in {
                "public/readme.txt": "public resource",
                ".DS_Store": "finder metadata",
                "public/.DS_Store": "nested finder metadata",
                ".recovery/private.txt": "previous work",
                ".state/private.txt": "runtime cache",
                ".iisacc-tests-private/private.txt": "temporary test data",
                "public/.recovery/nested.txt": "nested recovery",
            }.items():
                destination = fixture/relative
                provider.mkdir(destination.parent, parents=True, exist_ok=True)
                provider.write_text(destination, contents)
            module = (ROOT/"cmake/iiSharedCanvasInstallResources.cmake").as_posix()
            provider.mkdir(source/"src", parents=True, exist_ok=True)
            provider.write_text(source/"CMakeLists.txt",
                'cmake_minimum_required(VERSION 3.31)\nproject(ResourcePolicy LANGUAGES CXX)\n'
                'find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Contracts)\n'
                'add_library(ResourcePolicy INTERFACE)\n'
                'target_link_libraries(ResourcePolicy INTERFACE iiSDKCore::Contracts)\n'
                f'include("{module}")\n'
                'iisc_install_resources("${CMAKE_CURRENT_SOURCE_DIR}/fixtures" resources)\n')
            platform = "macos" if sys.platform == "darwin" else "windows" if os.name == "nt" else "linux"
            builder = os.environ.get("IISDKBUILDER_EXECUTABLE") or str(
                Path.home()/".local/SDK/iiSDKBuilder/bin"/("iisdk-builder.exe" if os.name == "nt" else "iisdk-builder"))
            core = os.environ.get("IISDKCORE_PREFIX") or str(Path.home()/".local/SDK/iiSDKCore")
            manifest = {
                "schema": 1, "mode": "library",
                "project": {"name": "ResourcePolicy", "source": str(source)},
                "buildRoot": str(build), "configuration": "Release",
                "kits": [{"id": "host-policy", "platform": platform, "prefixes": [core]}],
            }
            manifest_file = workspace/"manifest.json"
            provider.write_text(manifest_file, json.dumps(manifest))
            environment = dict(os.environ, TMPDIR=str(workspace), PYTHONDONTWRITEBYTECODE="1")
            subprocess.run([builder, "build", "--manifest", str(manifest_file)], env=environment,
                           capture_output=True, text=True, check=True)
            stage = build/"kits/host-policy/Release/prefix/ResourcePolicy"
            files = {p.relative_to(stage/"resources").as_posix()
                     for p in (stage/"resources").rglob("*") if p.is_file()}
            self.assertIn("public/readme.txt", files)
            self.assertFalse(any(part in {".recovery", ".state", ".DS_Store"} or part.startswith(".iisacc-")
                                 for file in files for part in Path(file).parts))
        finally:
            if build.is_dir() and not build.is_symlink():
                remove_owned(build)
            if workspace.is_dir() and not workspace.is_symlink():
                remove_owned(workspace)


if __name__ == "__main__":
    unittest.main()
