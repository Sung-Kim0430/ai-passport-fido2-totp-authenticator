"""检查当前 SDK 基线、锁文件与维护说明一致，不依赖第三方 YAML 解析器。"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def locked_version(text, name):
    # 只读取顶层组件的四空格 version，避免误取依赖约束。
    start = text.index(f"  {name}:\n")
    block = text[start:]
    end = re.search(r"\n  \S", block)
    if end:
        block = block[:end.start()]
    match = re.search(r"^    version: ['\"]?([^'\"\s]+)", block, re.MULTILINE)
    if not match:
        raise AssertionError(f"Missing locked version for {name}")
    return match.group(1)


class SdkVersionContract(unittest.TestCase):
    def setUp(self):
        manifest = (ROOT / "components/bsp/idf_component.yml").read_text()
        self.expected = re.search(r'idf: ">=(\d+\.\d+\.\d+),', manifest).group(1)
        self.helper = re.search(r'espressif/cmake_utilities: "([^"]+)"', manifest).group(1)

    def test_lock_matches_selected_versions(self):
        lock = (ROOT / "dependencies.lock").read_text()
        self.assertEqual(locked_version(lock, "idf"), self.expected)
        self.assertEqual(locked_version(lock, "espressif/cmake_utilities"), self.helper)

    def test_workflows_match_sdk(self):
        for name in ("build-firmware.yml", "firmware-checks.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                self.assertIn(f"esp_idf_version: v{self.expected}", text)

    def test_current_agent_guidance_matches_sdk(self):
        paths = (
            "AGENTS.md", "AGENTS.zh_CN.md",
            "skills/passport-setup/SKILL.md", "skills/passport-setup/SKILL.zh_CN.md",
            "skills/passport-build/SKILL.md", "skills/passport-build/SKILL.zh_CN.md",
        )
        for name in paths:
            with self.subTest(document=name):
                self.assertIn(self.expected, (ROOT / name).read_text())


if __name__ == "__main__":
    unittest.main()
