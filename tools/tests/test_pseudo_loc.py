"""Input validation and output checks for Qt pseudo localization."""

from pathlib import Path

import defusedxml.ElementTree as ET
import pytest
from pseudo_loc import process_ts


def test_wrong_root_preserves_destination(tmp_path: Path) -> None:
    source = tmp_path / "source.ts"
    destination = tmp_path / "output.ts"
    source.write_text("<unrelated/>")
    destination.write_text("existing output")
    with pytest.raises(ValueError, match="TS root"):
        process_ts(source, destination, "eo")
    assert destination.read_text() == "existing output"


def test_translates_text_and_preserves_format_markers(tmp_path: Path) -> None:
    source = tmp_path / "source.ts"
    destination = tmp_path / "output.ts"
    source.write_text("<TS><context><message><source>Hello %1</source></message></context></TS>")
    assert process_ts(source, destination, "eo") == 1
    root = ET.parse(destination).getroot()
    assert root is not None
    assert root.get("language") == "eo"
    translation = root.findtext("context/message/translation")
    assert translation is not None and "%1" in translation and "Hello" not in translation
