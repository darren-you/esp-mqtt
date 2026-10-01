"""用官方 Component Manager 验证 Git 元数据不会改变组件字节或摘要。"""

from pathlib import Path
import tempfile
import unittest

from idf_component_tools.file_tools import copy_filtered_directory
from idf_component_tools.hash_tools.calculate import hash_dir
from idf_component_tools.manager import ManifestManager


ROOT = Path(__file__).resolve().parents[1]


class ComponentHashTest(unittest.TestCase):
    def test_submodule_locator_is_excluded(self):
        manifest = ManifestManager(ROOT, "mqtt").load()
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            inputs, outputs = [], []
            for name in ("cache-a", "cache-b"):
                source = temporary / name
                submodule = source / "test/tools/paho.mqtt.testing"
                submodule.mkdir(parents=True)
                for relative in ("mqtt_client.c", "runtime/emqtt.c", "idf_component.yml"):
                    target = source / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes((ROOT / relative).read_bytes())
                (submodule / ".git").write_text(f"gitdir: /private/{name}/modules/paho\n")
                (submodule / "README.md").write_text("same pinned submodule bytes\n")
                destination = temporary / f"{name}-package"
                copy_filtered_directory(source, destination,
                    use_gitignore=manifest.use_gitignore,
                    include=manifest.include_set, exclude=manifest.exclude_set)
                self.assertFalse((destination / "test/tools/paho.mqtt.testing/.git").exists())
                self.assertTrue((destination / "test/tools/paho.mqtt.testing/README.md").is_file())
                for relative in ("mqtt_client.c", "runtime/emqtt.c", "idf_component.yml"):
                    self.assertEqual((destination / relative).read_bytes(),
                                     (ROOT / relative).read_bytes())
                inputs.append(source)
                outputs.append(destination)
            self.assertNotEqual(hash_dir(inputs[0]), hash_dir(inputs[1]))
            self.assertEqual(hash_dir(outputs[0]), hash_dir(outputs[1]))


if __name__ == "__main__":
    unittest.main()
