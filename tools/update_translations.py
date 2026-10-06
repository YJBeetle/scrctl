"""调用 GNU gettext 更新或检查消息目录，不自行解析 PO 文件。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description="Update or check English/Chinese message catalogs")
    parser.add_argument("--check", action="store_true", help="Check source messages without changing files")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]

    def run(tool, *arguments):
        subprocess.run([tool, *map(str, arguments)], cwd=root, check=True)

    with tempfile.TemporaryDirectory(prefix="scrctl-translations-") as directory:
        template = Path(directory) / "scrctl.pot"
        sources = sorted(str(p.relative_to(root)) for p in (root / "src").rglob("*")
                         if p.suffix in (".cpp", ".h"))
        run("xgettext", "--language=C++", "--from-code=UTF-8", "--sort-by-file",
            "--keyword=SCRCTL_TR", "--keyword=SCRCTL_N_",
            "--flag=SCRCTL_TR:1:pass-c-format", "--flag=SCRCTL_N_:1:pass-c-format",
            "--package-name=scrctl", "--package-version=0.1.0", "-o", template, *sources)
        if args.check:
            run("msgcmp", "--no-fuzzy-matching", "--use-untranslated", "po/scrctl.pot", template)
        else:
            shutil.copyfile(template, root / "po/scrctl.pot")
            run("msgmerge", "--update", "--backup=none", "--no-fuzzy-matching", "--sort-by-file",
                "po/zh_CN.po", "po/scrctl.pot")
        # msgcmp 要求每条源消息有确定译文；msgfmt 校验格式及 printf 占位符。
        run("msgcmp", "--no-fuzzy-matching", "po/zh_CN.po", template)
        run("msgfmt", "--check", "--check-format", "-o", Path(directory) / "scrctl.mo", "po/zh_CN.po")
    print("Message catalogs match source and all Chinese messages are translated")


if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError as error:
        print("Required gettext tool not found: " + str(error.filename), file=sys.stderr)
        sys.exit(1)
    except subprocess.CalledProcessError:
        print("Catalog check failed; update catalogs and fill missing translations", file=sys.stderr)
        sys.exit(1)
