from __future__ import annotations

from classify_s1117_signal_shadows import classify, find_signal_names
from sonar_issues import Issue


def test_find_signal_names_parses_signals_blocks(tmp_path):
    header = tmp_path / "widget.h"
    header.write_text(
        "class Widget : public QWidget {\n"
        "    Q_OBJECT\n"
        "public:\n"
        "    void notASignal(int x);\n"
        "signals:\n"
        "    void LOG_I(QString message, bool timestamp, bool linefeed);\n"
        "    void progressChanged(int done, int total);\n"
        "public slots:\n"
        "    void onClicked();\n"
        "};\n"
    )
    assert find_signal_names([str(header)]) == {"LOG_I", "progressChanged"}


def test_classify_splits_emit_artifact_from_remaining():
    issues = [
        Issue(
            key="a",
            rule="cpp:S1117",
            component="P:x.cpp",
            line=1,
            message='Declaration shadows a local variable "LOG_I" in the outer scope.',
        ),
        Issue(
            key="b",
            rule="cpp:S1117",
            component="P:x.cpp",
            line=2,
            message='Declaration shadows a local variable "item_local" in the outer scope.',
        ),
    ]
    emit_artifact, remaining = classify(issues, signal_names={"LOG_I"})
    assert [i.key for i in emit_artifact] == ["a"]
    assert [i.key for i in remaining] == ["b"]
