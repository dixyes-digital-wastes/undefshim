#!/usr/bin/env python3
"""The structure the tree is supposed to have, checked against the tree.

Every rule here is one that was written down in the design document and then
broken anyway, because nothing enforced it. They are the kind whose violation
is silent: a header guard that collides with another makes the second header
disappear, a core file that reaches into the firmware compiles until the day it
is built for the payload, and a milestone renamed in the table leaves a script
waiting for a line that is never printed.

    python3 check_structure.py [root]

Each rule answers with the files and lines that break it, so a failure says
where to look rather than that something is wrong somewhere. The rules and what
they are for:

  guards        Two headers with one guard is one header, silently
  layers        core/ and payload/ are built for a world with no firmware
  ownership     Every core file says which side it is compiled into, and says
                the same thing as the Makefile
  config        Every key a shipped configuration uses is one the parser knows,
                since an unknown one is ignored rather than reported
  milestones    Every milestone a script waits for is one the boot can print
  tree          A path the design document's listing names is there
  deadcode      A declaration in a first party header with no user anywhere

Everything is read as text. That is enough for all of them - a guard is a line,
an include is a line, a key is a string and a milestone is a table entry - and
it is what lets this run before the compiler, on a tree that does not build.
"""

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check as checklib

REPORT = checklib.Check("check_structure")

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
DOCS = (ROOT.parent / "undefshim-ex2-docs").resolve()

FIRST_PARTY = ("core", "common", "uefi", "payload")
SOURCE_SUFFIXES = (".c", ".h", ".S")

# Declarations nothing is expected to call. The project keeps a few on
# purpose, and each one is a claim made here rather than a silence. Entry
# points the linker is told to keep are absent because those are definitions
ALLOWED_UNUSED = set()

failures = 0
checks = 0


def fail(rule, message):
    global failures
    failures += 1
    REPORT.fail("{}: {}".format(rule, message))


def ok(rule, message):
    global checks
    checks += 1
    REPORT.note("{}: {}".format(rule, message))


def sources(where):
    """Every first party source under a directory, sorted."""
    return sorted(p for p in (ROOT / where).rglob("*")
                  if p.suffix in SOURCE_SUFFIXES and p.is_file())


def headers():
    return [p for d in FIRST_PARTY for p in sources(d) if p.suffix == ".h"]


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def rel(path):
    return str(Path(path).relative_to(ROOT))


def lineOf(text, needle):
    """The 1 based line a substring is on, for a message that can be read."""
    at = text.find(needle)
    return text.count("\n", 0, at) + 1 if at >= 0 else 0


# --- guards ---------------------------------------------------------------

def checkGuards():
    """One guard per header, and not shared with another header.

    A guard is a name in a global namespace. Two headers that pick the same one
    are one header: the second is skipped wherever the first was already read,
    which is a set of declarations missing in exactly the translation units
    that include both, and nothing says so
    """
    seen = {}
    for header in headers():
        text = read(header)
        match = re.search(r"^#define\s+(US_[A-Z0-9_]*_H)\s*$", text, re.M)
        if match is None:
            fail("guards", "{}: no include guard".format(rel(header)))
            continue
        guard = match.group(1)
        if guard in seen:
            fail("guards", "{}: guard {} is already {}'s".format(
                rel(header), guard, rel(seen[guard])))
            continue
        seen[guard] = header
        # The guard has to be this file's own name. That is the rule a shared
        # stem breaks: uefi/vamap.h and payload/vamap.h both reached for
        # US_VAMAP_H, and the second one read was skipped wherever the first
        # had been. A header may add its directory to make the name unique,
        # which is why the check is a prefix rather than an equality
        # The letters of the file's name have to be in the guard, in order.
        # That is the rule a shared stem breaks: uefi/vamap.h and
        # payload/vamap.h both reached for US_VAMAP_H, and whichever was read
        # second was skipped for the rest of that translation unit. Letters
        # rather than pieces, so that a name written without separators -
        # panelface.h and US_PANEL_FACE_H - is read as the same name
        letters = [c for c in header.stem.upper() if c != "_"]
        at = 0
        for letter in letters:
            found = guard.find(letter, at)
            if found < 0:
                fail("guards", "{}: guard {} does not name this file".format(
                    rel(header), guard))
                break
            at = found + 1
    ok("guards", "{} headers, each with its own guard".format(len(seen)))


# --- layers ---------------------------------------------------------------

# Layer -> the directories its sources may not include from. The rule is the
# design document's: core and payload are built for a world with no firmware,
# and a file that reaches into uefi/ compiles until the day it is built for the
# payload
FORBIDDEN = {
    "core": ("uefi/",),
    "payload": ("uefi/",),
    "common": ("uefi/", "core/", "payload/"),
}

# Headers no core file may include, whatever the include path is: the trees
# that only exist on the driver's side
THIRD_PARTY_FOR_CORE = ("toml.h", "uefi.h", "uacpi/", "tlsf.h")


def includesOf(path):
    """(line, target) for every quoted include in a file."""
    out = []
    for number, line in enumerate(read(path).splitlines(), 1):
        match = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
        if match is not None:
            out.append((number, match.group(1)))
    return out


def checkLayers():
    """core/ and payload/ do not know about the firmware or about each other.

    The direction is core and payload -> common, and uefi -> core, payload and
    common. A quoted include that points the other way has to be caught here,
    because the ordinary build does not catch it: core is compiled twice and
    the driver's copy has the firmware headers on its include path
    """
    broken = 0
    counted = 0
    for top, forbidden in FORBIDDEN.items():
        for source in sources(top):
            counted += 1
            for number, target in includesOf(source):
                if any(target.startswith(prefix) for prefix in forbidden):
                    fail("layers", "{}:{}: includes {}".format(rel(source), number,
                                                               target))
                    broken += 1
                # The parser is reached from the file that parses, not from
                # the header that describes the result: a core *header* that
                # names a third party tree is one every translation unit that
                # includes it has to have that tree's include path for
                if (top == "core" and source.suffix == ".h"
                        and any(target == name or target.startswith(name)
                                for name in THIRD_PARTY_FOR_CORE)):
                    fail("layers", "{}:{}: core reaches {}".format(
                        rel(source), number, target))
                    broken += 1
    ok("layers", "{} sources, {} includes the wrong way".format(counted, broken))


# --- ownership ------------------------------------------------------------

# The line a core source carries to say which side it is compiled into. A
# comment rather than something in the build, because the build is where the
# fact is duplicated: this is the same claim written where the file is
OWNERSHIP = re.compile(r"^// us-build: (both|driver|payload)\s*$", re.M)


def makefileSets():
    """The core sources in each of the Makefile's two lists.

    Read as text: the lists are the interface between this check and the build,
    and a build that has stopped naming a file here is the failure being looked
    for
    """
    text = read(ROOT / "Makefile")
    found = {}
    for name in ("PAYLOAD_SRCS", "DRIVER_SRCS"):
        match = re.search(r"^\s*{}\s*:=(.*?)(?=\n\s*[A-Z_]+\s*:?=|\n\n)".format(name),
                          text, re.S | re.M)
        if match is None:
            fail("ownership", "Makefile: {} not found".format(name))
            found[name] = set()
            continue
        found[name] = set(re.findall(r"core/([a-z0-9_]+\.c)", match.group(1)))
    return found["PAYLOAD_SRCS"], found["DRIVER_SRCS"]


def checkOwnership():
    """Every core file says which sides it is built into, and agrees with make.

    The claim in the file and the claim in the Makefile are two statements of
    one fact, which is the arrangement this rule keeps honest: a file added to
    the payload list without its marker, or marked one way and listed the
    other, is a file compiled for a world it was not written for
    """
    payload, driver = makefileSets()
    if not payload and not driver:
        return

    marked = {}
    for source in sources("core"):
        if source.suffix != ".c":
            continue
        match = OWNERSHIP.search(read(source))
        if match is None:
            fail("ownership", "{}: no us-build: line".format(rel(source)))
            continue
        marked[source.name] = match.group(1)

    for name in sorted(marked):
        where = marked[name]
        inPayload = name in payload
        inDriver = name in driver
        if where == "both":
            agrees = inPayload and inDriver
        elif where == "payload":
            agrees = inPayload and not inDriver
        else:
            agrees = inDriver and not inPayload
        if not agrees:
            fail("ownership", "core/{} says {} and the Makefile says payload={} "
                 "driver={}".format(name, where, inPayload, inDriver))

    for name in sorted(payload | driver):
        if name not in marked:
            fail("ownership", "core/{} is built but carries no marker".format(name))
    for name in sorted(marked):
        if name not in payload and name not in driver:
            fail("ownership", "core/{} is marked but in neither list".format(name))

    ok("ownership", "{} core files, {} in the payload, {} in the driver".format(
        len(marked), len(payload), len(driver)))


# --- config ---------------------------------------------------------------

def configKeys():
    """The keys core/cfg.c accepts, read out of its own calls.

    The parser names a key in a string and reads it into a field, so the set of
    keys is the set of those strings. Reading them rather than listing them
    means a key added to the parser is covered without this file changing
    """
    text = read(ROOT / "core/cfg.c")
    keys = set(re.findall(
        r"\bcfg[A-Za-z0-9_]*\s*\(\s*[a-zA-Z_]+\s*,\s*\"([^\"]+)\"", text))
    # A key read straight out of the table rather than through a helper: the
    # parser does both, and a key it reads is a key it knows
    keys |= set(re.findall(r'toml_table_[a-z_]+(?:_ref)?\s*\(\s*[a-z]+\s*,\s*"([^"]+)"',
                           text))
    return keys


def checkConfig():
    """Every key in a shipped configuration is one the parser knows.

    An unknown key is ignored rather than reported, which is right for a file
    written for a newer build and wrong for a typo: the machine runs with the
    default and the switch that was meant to be set was not
    """
    known = configKeys()
    if not known:
        fail("config", "core/cfg.c: no keys could be read")
        return

    files = [ROOT / "config/us.toml", ROOT / "config/us.toml.d920s10",
             ROOT / "tests/fake/us.toml"]
    checked = 0
    for path in files:
        if not path.exists():
            continue
        checked += 1
        section = ""
        for number, line in enumerate(read(path).splitlines(), 1):
            line = line.split("#", 1)[0].strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            match = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
            if match is None:
                continue
            if match.group(1) not in known:
                fail("config", "{}:{}: [{}] {} is not a key the parser reads"
                     .format(rel(path), number, section, match.group(1)))
    ok("config", "{} keys read, {} configurations checked".format(len(known),
                                                                 checked))


# --- milestones -----------------------------------------------------------

def milestoneNames():
    """The lines the boot can print, read out of the stage table."""
    text = read(ROOT / "uefi/stage.c")
    return set(re.findall(r'\[UsStage[A-Za-z0-9_]+\]\s*=\s*"([^"]+)"', text))


def checkMilestones():
    """Every milestone a deploy script waits for is one the boot can print.

    The scripts and the boot agree on a string and on nothing else, so a
    renamed stage is a run that waits until it times out. What the scripts
    spell out are their grep patterns, so the alternatives in those patterns
    are what is looked up - which is also what catches a script that greps
    for a name to its end and would now match nothing
    """
    known = milestoneNames()
    if not known:
        fail("milestones", "uefi/stage.c: no names could be read")
        return

    # A bare "M5" or "M6.5" is a part of the boot's own numbering that a
    # script greps for with alternatives around it, not a name
    numbering = re.compile(r"^M[0-9]+(\.[0-9]+)?$")

    patterns = re.compile(r"'([^']*M[0-9][^']*)'|\"([^\"]*M[0-9][^\"]*)\"")
    # A milestone is a word, so what is looked for is an alternative that
    # starts like one: the letter M, a number, and then a word
    candidate = re.compile(r"\bM[0-9][0-9.]*\s+[A-Za-z][A-Za-z0-9. ]*")

    deploy = ROOT / "tests" / "deploy"
    scripts = 0
    waited = set()
    for path in sorted(deploy.glob("*.sh")):
        scripts += 1
        text = "\n".join(line for line in read(path).splitlines()
                         if not line.lstrip().startswith("#"))
        for match in patterns.finditer(text):
            quoted = match.group(1) if match.group(1) is not None else match.group(2)
            for alternative in quoted.split("|"):
                for found in candidate.finditer(alternative):
                    name = found.group(0).rstrip()
                    if numbering.match(name):
                        continue
                    waited.add(name)
                    if name not in known:
                        fail("milestones",
                             "{}: waits for \"{}\", which the boot cannot print"
                             .format(rel(path), name))
    ok("milestones", "{} names, {} scripts, {} waited for".format(
        len(known), scripts, len(waited)))


# --- payload addresses ----------------------------------------------------

# A structure that carries function pointers, in a header the payload
# includes. Initialising one as an aggregate literal is the trap: the compiler
# puts a copy in .rodata and the linker resolves the function addresses at link
# time, so after the blob is copied they point at the address the blob had when
# it was linked. Nothing in the object file says so - there is no relocation -
# and the boot's own link check is what catches it, late
FUNCTION_POINTER_STRUCTS = ("UsRcpcScan",)

AGGREGATE_INIT = re.compile(r"\b({})\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*\{{[^}}]*\.[A-Za-z]"
                            .format("|".join(FUNCTION_POINTER_STRUCTS)))


def checkPayloadAggregates():
    """No aggregate literal of a structure that holds a function address.

    A file that does this still links, still has no relocations, and still
    passes every host test: what it breaks is the copy, and only the payload's
    own link check sees it. The rule is here so that the mistake is a line of
    output from a check that runs in a second, rather than a red build in the
    middle of something else
    """
    found = 0
    for top in ("core", "payload", "uefi"):
        for source in sources(top):
            if source.suffix != ".c":
                continue
            text = read(source)
            for match in AGGREGATE_INIT.finditer(text):
                fail("payload", "{}:{}: {} initialised as an aggregate, which "
                     "the compiler turns into a copy of link time addresses"
                     .format(rel(source), lineOf(text, match.group(0)),
                             match.group(1)))
                found += 1
    ok("payload", "{} structures watched, {} aggregate literals".format(
        len(FUNCTION_POINTER_STRUCTS), found))


# --- tree -----------------------------------------------------------------

# A line of the listing: the runners and then the name. A line with a nearer
# prefix is a child of the nearest line above it with a shorter one, which is
# what the runners mean and all that has to be understood to read the paths
TREE_LINE = re.compile(r"^([\s│]*)(?:├──|└──)?\s*([A-Za-z0-9_][A-Za-z0-9_.,{}-]*)")


def designTreePaths():
    """The paths the design document's directory listing names.

    The listing is a fenced block in the section that describes the layout, and
    the paths in it are the ones a reader believes exist. It is read as the tree
    it is drawn as: indentation decides what a name is inside, so a line that
    was moved without its children moving is still read as the file it is
    """
    text = read(DOCS / "design.md")
    lines = text.splitlines()
    begin = None
    for number, line in enumerate(lines):
        if line.startswith("## ") and "目录布局" in line:
            begin = number
            break
    if begin is None:
        fail("tree", "design.md: the layout section was not found")
        return set()

    inside = False
    stack = []
    paths = set()
    for line in lines[begin:]:
        if line.startswith("```"):
            if inside:
                break
            inside = True
            continue
        if not inside:
            continue
        match = TREE_LINE.match(line)
        if match is None:
            continue
        prefix = match.group(1)
        name = match.group(2)
        # How deep the line sits: one level per four columns of runners
        depth = len(prefix.expandtabs(4)) // 4
        while stack and stack[-1][0] >= depth:
            stack.pop()
        parent = stack[-1][1] if stack else ""
        stack.append((depth, (parent + "/" + name) if parent else name))
        # A source listed as its pair, "scan.{h,c}", names two files the reader
        # looks for
        if name.endswith("}") and "{" in name:
            head, _, rest = name.partition("{")
            for extension in rest.rstrip("}").split(","):
                paths.add((parent + "/" if parent else "") + head + extension)
        elif "." in name:
            paths.add((parent + "/" if parent else "") + name)
    return paths


def checkTree():
    """A path the design document lists has to be there.

    The listing is what a reader uses to find their way around, so a file that
    moved without the listing moving sends them to a file that is not there.
    The other direction is not a failure: a file the listing does not mention
    is a gap in the document rather than a lie in it
    """
    paths = designTreePaths()
    if not paths:
        fail("tree", "design.md: no paths could be read")
        return

    missing = 0
    for path in sorted(paths):
        if path.startswith("undefshim-ex2/"):
            path = path[len("undefshim-ex2/"):]
        if not (ROOT / path).exists():
            fail("tree", "design.md lists {}, which is not there".format(path))
            missing += 1
    ok("tree", "{} listed paths, {} missing".format(len(paths), missing))


# --- dead code ------------------------------------------------------------

DECL = re.compile(r"^(?:[A-Za-z_][A-Za-z0-9_ \t*]*?\b)"
                  r"((?:us|Us)[A-Za-z0-9_]+)\s*\([^;{]*\)\s*;")


def checkDeadDeclarations():
    """A function declared in a first party header and called nowhere.

    A report and not a rule: it is here because a declaration with no caller is
    how a removed mechanism leaves its interface behind. Definitions are looked
    for across the whole tree, the forks included, because a declaration may be
    satisfied by a file only one of the two builds compiles
    """
    body = {}
    for directory in FIRST_PARTY + ("tests",):
        for path in (ROOT / directory).rglob("*"):
            if path.suffix in (".c", ".h", ".S") and path.is_file():
                body[path] = read(path)

    unused = []
    walked = 0
    for header in headers():
        text = read(header)
        walked += 1
        for match in DECL.finditer(text):
            name = match.group(1)
            if name in ALLOWED_UNUSED:
                continue
            users = 0
            for path, content in body.items():
                if path == header:
                    continue
                for hit in re.finditer(r"\b{}\s*\(".format(re.escape(name)), content):
                    line = content[content.rfind("\n", 0, hit.start()) + 1:
                                   content.find("\n", hit.start())]
                    stripped = line.strip()
                    # A declaration, not a call: what is counted is a use
                    if stripped.endswith(";") and "=" not in stripped:
                        continue
                    users += 1
            if users == 0:
                unused.append("{}:{}: {}".format(rel(header),
                                                 lineOf(text, match.group(0)), name))
    if unused:
        REPORT.note("deadcode: declarations with no user:")
        for line in unused:
            REPORT.note("  " + line)
    ok("deadcode", "{} headers walked, {} declarations with no user".format(
        walked, len(unused)))


def main():
    checkGuards()
    checkLayers()
    checkOwnership()
    checkConfig()
    checkMilestones()
    checkPayloadAggregates()
    checkTree()
    checkDeadDeclarations()
    return REPORT.summary(checks, failures)


if __name__ == "__main__":
    sys.exit(main())
