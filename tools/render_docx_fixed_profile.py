import importlib.util
import os
import shutil
import sys
import uuid
from pathlib import Path


RENDERER = Path(
    r"C:\Users\017691\.codex\plugins\cache\openai-primary-runtime\documents\26.715.12143\skills\documents\render_docx.py"
)
DOCX = Path(r"D:\freertos_study\FreeRTOS学习总笔记.docx")
OUT_DIR = Path(r"D:\freertos_study\render_total_notes")
TMP_ROOT = Path(r"D:\freertos_study\tmp")


def load_renderer():
    spec = importlib.util.spec_from_file_location("codex_render_docx", RENDERER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def main():
    renderer = load_renderer()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    TMP_ROOT.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex[:10]
    profile_dir = TMP_ROOT / f"fixed_soffice_profile_{token}"
    convert_dir = TMP_ROOT / f"fixed_soffice_convert_{token}"
    profile_dir.mkdir(parents=True, exist_ok=True)
    convert_dir.mkdir(parents=True, exist_ok=True)

    os.environ.pop("XDG_CONFIG_HOME", None)
    os.environ.pop("XDG_CACHE_HOME", None)
    os.environ["TMP"] = str(TMP_ROOT)
    os.environ["TEMP"] = str(TMP_ROOT)
    os.environ["HOME"] = str(profile_dir)

    stem = DOCX.stem
    pdf_path, debug = renderer.convert_to_pdf(
        str(DOCX), str(profile_dir), str(convert_dir), stem, verbose=True
    )
    if not pdf_path or not Path(pdf_path).exists():
        raise RuntimeError("PDF conversion failed:\n" + debug)

    dst_pdf = OUT_DIR / f"{stem}.pdf"
    shutil.copy2(pdf_path, dst_pdf)
    paths_raw = renderer.convert_from_path(
        pdf_path,
        dpi=144,
        fmt="png",
        thread_count=4,
        output_folder=str(OUT_DIR),
        paths_only=True,
        output_file="page",
    )
    pages = []
    for src in paths_raw:
        src_path = Path(src)
        page_num = int(src_path.stem.split("-")[-1])
        dst_path = OUT_DIR / f"page-{page_num}.png"
        if dst_path.exists():
            dst_path.unlink()
        src_path.replace(dst_path)
        pages.append(dst_path)
    print(f"pdf={dst_pdf}")
    print(f"pages={len(pages)}")
    for page in sorted(pages, key=lambda p: int(p.stem.split('-')[-1])):
        print(page)


if __name__ == "__main__":
    sys.exit(main())
