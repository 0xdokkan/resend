#!/usr/bin/env python3
"""Brings the translation catalogs up to date with the source.

Collects every text written for people, @tr("...") in ui/*.slint,
tr("...") in src/ and the titles and notes of examples/*.json, and rewrites translations/<language>/LC_MESSAGES/resend.po
for every language: translations already made are kept, new texts get an
empty translation to fill in, texts no longer used are dropped. The
translation tests (tests/translation_tests.cpp) fail until every text is
translated.

Usage: scripts/update-translations.py [--check]
  --check  only report what would change; exit 1 if a catalog is out of date
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}


def literal(source, at):
    """The string literal starting at `at`, and the index after it."""
    value, at = "", at + 1
    while source[at] != '"':
        if source[at] == "\\":
            value += ESCAPES.get(source[at + 1], source[at + 1])
            at += 2
        else:
            value += source[at]
            at += 1
    return value, at + 1


def skip_space(source, at):
    while at < len(source):
        if source[at].isspace():
            at += 1
        elif source.startswith("//", at):
            at = source.find("\n", at)
            if at < 0:
                return len(source)
        else:
            break
    return at


def collect():
    """English texts in the order they appear, each with its files."""
    texts = {}
    for folder, extensions in (("src", (".cpp", ".hpp")), ("ui", (".slint",))):
        for directory, _, names in sorted(os.walk(os.path.join(ROOT, folder))):
            for name in sorted(names):
                if not name.endswith(extensions):
                    continue
                path = os.path.join(directory, name)
                with open(path, encoding="utf-8") as file:
                    source = file.read()
                slint = name.endswith(".slint")
                pattern = r"@tr\(" if slint else r"(?<![A-Za-z0-9_@])tr\("
                for match in re.finditer(pattern, source):
                    at = skip_space(source, match.end())
                    if at >= len(source) or source[at] != '"':
                        continue
                    english = ""
                    while at < len(source) and source[at] == '"':
                        part, at = literal(source, at)
                        english += part
                        if slint:
                            break
                        at = skip_space(source, at)
                    files = texts.setdefault(english, [])
                    relative = os.path.relpath(path, ROOT).replace(os.sep, "/")
                    if relative not in files:
                        files.append(relative)
    examples = os.path.join(ROOT, "examples")
    for name in sorted(os.listdir(examples)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(examples, name), encoding="utf-8") as file:
            scenario = json.load(file)
        for key in ("title", "notes"):
            if scenario.get(key):
                texts.setdefault(scenario[key], []).append("examples/" + name)
    return texts


def unquote(line):
    value, _ = literal(line, line.index('"'))
    return value


def read_po(path):
    """The header lines and the translations of a .po file."""
    header, translations = [], {}
    with open(path, encoding="utf-8") as file:
        lines = file.read().split("\n")
    entry, field, seen_id = {}, None, False
    for line in lines + [""]:
        line = line.strip()
        if not seen_id and not line.startswith("msgid") and not entry:
            if line.startswith("#") and not line.startswith("#:"):
                header.append(line)
            if line.startswith("#,"):
                entry["fuzzy"] = "fuzzy" in line
            continue
        if not line:
            if "msgid" in entry and entry["msgid"]:
                translations[entry["msgid"]] = "" if entry.get("fuzzy") else entry.get("msgstr", "")
            entry, field = {}, None
            continue
        seen_id = True
        if line.startswith("#,"):
            entry["fuzzy"] = "fuzzy" in line
        elif line.startswith("#"):
            continue
        elif line.startswith("msgid "):
            field = "msgid"
            entry[field] = unquote(line)
        elif line.startswith("msgstr "):
            field = "msgstr"
            entry[field] = unquote(line)
        elif line.startswith('"') and field:
            entry[field] += unquote(line)
    return header, translations


def quote(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def write_po(language, header, texts, translations):
    lines = header or [f"# {language} translation of resend."]
    lines += [
        'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: resend\\n"',
        f'"Language: {language}\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"',
        "",
    ]
    for english, files in texts.items():
        lines.append("#: " + " ".join(files))
        lines.append("msgid " + quote(english))
        lines.append("msgstr " + quote(translations.get(english, "")))
        lines.append("")
    return "\n".join(lines)


def main():
    check = "--check" in sys.argv[1:]
    texts = collect()
    folder = os.path.join(ROOT, "translations")
    outdated = False
    for language in sorted(os.listdir(folder)):
        path = os.path.join(folder, language, "LC_MESSAGES", "resend.po")
        if not os.path.isfile(path):
            continue
        header, translations = read_po(path)
        missing = [english for english in texts if not translations.get(english)]
        unused = [english for english in translations if english not in texts]
        updated = write_po(language, header, texts, translations)
        with open(path, encoding="utf-8") as file:
            changed = file.read() != updated
        print(f"{language}: {len(texts)} texts, {len(missing)} to translate, {len(unused)} no longer used")
        if changed and not check:
            with open(path, "w", encoding="utf-8") as file:
                file.write(updated)
        outdated = outdated or changed
    if check and outdated:
        print("Out of date: run scripts/update-translations.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
