#!/usr/bin/env python3
"""Unit tests for the dependency-isolated Debian package manifest."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("build.py")
SPEC = importlib.util.spec_from_file_location("pipewireao_deb_build", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class PackageManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = BUILD.load_manifest()
        self.components = self.manifest["components"]

    def test_package_names_and_install_tags_are_unique(self) -> None:
        packages = [component["package"] for component in self.components.values()]
        tags = [tag for component in self.components.values() for tag in component["install_tags"]]
        self.assertEqual(len(packages), len(set(packages)))
        self.assertEqual(len(tags), len(set(tags)))

    def test_every_optional_feature_has_one_package(self) -> None:
        owners: dict[str, list[str]] = {feature: [] for feature in self.manifest["feature_options"]}
        for name, component in self.components.items():
            for feature in component.get("enable", []):
                owners[feature].append(name)
        self.assertEqual({feature: 1 for feature in owners}, {feature: len(names) for feature, names in owners.items()})

    def test_component_options_disable_unselected_features(self) -> None:
        for component in self.components.values():
            options = BUILD.meson_options(self.manifest, component)
            values = dict(option.removeprefix("-D").split("=", 1) for option in options)
            enabled = set(component.get("enable", []))
            self.assertEqual(
                {feature for feature, value in values.items() if value == "enabled"}, enabled
            )
            self.assertTrue(all(value in {"enabled", "disabled"} for value in values.values()))

    def test_vendor_packages_require_private_runtime_metadata(self) -> None:
        vendors = {
            "alpao",
            "egrabber",
            "bgapi2",
            "edtpdv",
            "flisdk",
            "andor3",
            "hamamatsu",
            "hermes",
        }
        self.assertEqual(
            {name for name, component in self.components.items() if component.get("private_runtime")},
            vendors,
        )

    def test_optional_packages_depend_on_core(self) -> None:
        for name, component in self.components.items():
            self.assertEqual(component.get("depends_on_core", False), name not in {"core", "dev"})

    def test_core_is_the_only_component_that_builds_all_default_targets(self) -> None:
        self.assertEqual(self.components["core"]["build_targets"], [])
        self.assertIsNone(self.components["dev"]["build_targets"])
        for name, component in self.components.items():
            if name not in {"core", "dev"}:
                self.assertTrue(component["build_targets"])

    def test_component_feature_cannot_be_overridden(self) -> None:
        with self.assertRaises(SystemExit):
            BUILD.validate_meson_overrides(self.manifest, ["fits=auto"])
        BUILD.validate_meson_overrides(self.manifest, ["egrabber-prefix=/opt/euresys/egrabber"])

    def test_alpao_package_builds_sink_and_fgn_operator(self) -> None:
        self.assertEqual(
            self.components["alpao"]["build_targets"],
            ["spa-alpao", "alpao-fgn"],
        )

    def test_runtime_payload_cannot_escape_usr_lib(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            allowed = root / "usr" / "lib" / "plugin.so"
            allowed.parent.mkdir(parents=True)
            allowed.touch()
            self.assertEqual(BUILD.validate_payload(root, self.components["core"]), [allowed])
            forbidden = root / "opt" / "vendor" / "sdk.so"
            forbidden.parent.mkdir(parents=True)
            forbidden.touch()
            with self.assertRaises(SystemExit):
                BUILD.validate_payload(root, self.components["core"])


if __name__ == "__main__":
    unittest.main()
