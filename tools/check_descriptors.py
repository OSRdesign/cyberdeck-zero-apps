#!/usr/bin/env python3
"""Check the descriptor (summary + description) of every app and every registry entry.

    python3 tools/check_descriptors.py            # exit 0 when all pass, 1 otherwise

The descriptor is what Settings > Apps shows: the summary line and the description, joined by a newline. Rules:
  - summary: one line, not empty, at most 80 characters (it is shown elided in a list row);
  - summary + "\\n" + description: at most 500 characters (characters, not bytes). The launcher shows no more: it
    cuts a longer text at 500 with dots.
The long documentation of an app lives in its README.md in this repository, for reading on a computer.

Checked: apps/<id>/app.json of every app folder (published, draft and the template alike; a missing description
counts as the summary, as tools/make_registry.py does) and every entry of registry.json and registry-amd64.json
(the top-level text and its locales / i18n copies). tools/make_registry.py runs the same check on the apps it builds.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SUMMARY_MAX = 80
DESCRIPTOR_MAX = 500
REGISTRY_FILES = ("registry.json", "registry-amd64.json")


def descriptor_errors(summary, description):
    """The rule violations of one summary/description pair, as short messages (empty list: compliant)."""
    errors = []
    summary = summary or ""
    description = summary if description is None else description
    if not summary.strip():
        errors.append("summary is empty")
    if "\n" in summary:
        errors.append("summary is more than one line")
    if len(summary) > SUMMARY_MAX:
        errors.append("summary is %d characters (max %d)" % (len(summary), SUMMARY_MAX))
    total = len(summary) + 1 + len(description)
    if total > DESCRIPTOR_MAX:
        errors.append("summary + newline + description is %d characters (max %d)" % (total, DESCRIPTOR_MAX))
    return errors


def manifest_errors(meta):
    """descriptor_errors() of an app.json (description defaults to the summary)."""
    return descriptor_errors(meta.get("summary", ""), meta.get("description"))


def check_repo(repo=REPO):
    """Every problem found in the apps and registries of a repository, as "where: message" lines."""
    problems = []
    apps_dir = os.path.join(repo, "apps")
    for name in sorted(os.listdir(apps_dir)):
        path = os.path.join(apps_dir, name, "app.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as handle:
            meta = json.load(handle)
        problems += ["apps/%s/app.json: %s" % (name, e) for e in manifest_errors(meta)]
    for registry_file in REGISTRY_FILES:
        path = os.path.join(repo, registry_file)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as handle:
            registry = json.load(handle)
        for entry in registry.get("apps", []):
            where = "%s: %s" % (registry_file, entry.get("share_code", "?"))
            texts = [("", entry)]
            for group in ("locales", "i18n"):
                texts += [(" %s.%s" % (group, lang), t) for lang, t in (entry.get(group) or {}).items()]
            for suffix, text in texts:
                problems += ["%s%s: %s" % (where, suffix, e)
                             for e in descriptor_errors(text.get("summary", ""), text.get("description"))]
    return problems


def main():
    problems = check_repo()
    for line in problems:
        print(line)
    if problems:
        print("descriptors: %d problem(s); the rule: summary <= %d characters, summary + description <= %d characters "
              "(the long text goes in the app's README)" % (len(problems), SUMMARY_MAX, DESCRIPTOR_MAX), file=sys.stderr)
        return 1
    print("descriptors: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
