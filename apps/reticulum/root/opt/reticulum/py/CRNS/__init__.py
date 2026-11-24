compiling = False
noticed = False
notice_delay = 0.3
import os
import sys
import time
import threading
from importlib.util import find_spec
if find_spec("pyximport") and find_spec("cython"):
    import json
    import shutil
    import subprocess
    from importlib.abc import MetaPathFinder
    from importlib.machinery import ExtensionFileLoader, SourceFileLoader
    from importlib.machinery import EXTENSION_SUFFIXES
    from importlib.util import spec_from_file_location
    from distutils.extension import Extension
    from pyximport import pyxbuild

    COMPILE_DIRECTIVES = { "annotation_typing": False }

    _WORKER_SCRIPT = r"""
import sys, os, json
from distutils.extension import Extension
from pyximport import pyxbuild
def main():
    tasks_path = sys.argv[1]
    with open(tasks_path, "r") as fh: tasks = json.load(fh)
    results = []
    for task in tasks:
        try:
            os.makedirs(task["scratch"], exist_ok=True)
            extension = Extension(name=task["basename"], sources=[task["source"]])
            extension.cython_directives = task["directives"]
            so_path = pyxbuild.pyx_to_dll(task["source"], ext=extension, build_in_temp=True, pyxbuild_dir=task["scratch"])
            os.makedirs(os.path.dirname(task["target"]), exist_ok=True)
            os.replace(so_path, task["target"])
            results.append({"fullname": task["fullname"], "ok": True, "error": None})
        except Exception as e:
            results.append({"fullname": task["fullname"], "ok": False,
                            "error": "{}: {}".format(type(e).__name__, e)})
    with open(tasks_path + ".results", "w") as fh: json.dump(results, fh)
if __name__ == "__main__": main()
"""

    def _compile_jobs():
        jobs = os.environ.get("CRNS_JOBS")
        if jobs: return max(1, int(jobs))
        return max(1, min(os.cpu_count() or 1, 16))

    class _ImportFinder(MetaPathFinder):

        def __init__(self):
            self.build_dir = os.path.join(os.path.expanduser("~"), ".pyxbld")
            self.mirror_root = os.path.join(self.build_dir, "rnslib")
            self.source_root = self._find_source_root()
            self.abi_suffix = EXTENSION_SUFFIXES[0]
            self.unbuildable = {}
            self.unbuildable_path = os.path.join(self.build_dir, "unbuildable.json")
            self._verify_mirror_directives()
            try:
                with open(self.unbuildable_path, "r") as fh: self.unbuildable = json.load(fh)
            except Exception: self.unbuildable = {}

        def _verify_mirror_directives(self):
            # If the compiler directives have changed since the mirror was
            # built, the entire mirror is invalidated and rebuilt.
            stamp_path = os.path.join(self.build_dir, "rnslib.stamp")
            directives = json.dumps(COMPILE_DIRECTIVES, sort_keys=True)
            stamp = directives + "\n" + self.abi_suffix
            previous = None
            try:
                with open(stamp_path, "r") as fh: previous = fh.read()
            except Exception: pass
            if previous != stamp:
                shutil.rmtree(self.mirror_root, ignore_errors=True)
                os.makedirs(self.mirror_root, exist_ok=True)
                try:
                    os.makedirs(self.build_dir, exist_ok=True)
                    with open(stamp_path, "w") as fh: fh.write(stamp)
                except Exception: pass

        def _save_unbuildable(self):
            try:
                os.makedirs(os.path.dirname(self.unbuildable_path), exist_ok=True)
                with open(self.unbuildable_path, "w") as fh: json.dump(self.unbuildable, fh, indent=1)
            except Exception: pass

        def _mark_unbuildable(self, fullname, source):
            try:
                self.unbuildable[fullname] = int(os.path.getmtime(source))
                self._save_unbuildable()
            except Exception: pass

        def _unmark_buildable(self, fullname):
            if fullname in self.unbuildable:
                del self.unbuildable[fullname]
                self._save_unbuildable()

        def _is_unbuildable(self, fullname, source):
            if fullname not in self.unbuildable: return False
            try:
                if int(os.path.getmtime(source)) == self.unbuildable[fullname]: return True
            except Exception: pass
            self.unbuildable.pop(fullname, None)
            return False

        def _find_source_root(self):
            here = os.path.dirname(os.path.abspath(__file__))
            candidate = os.path.abspath(os.path.join(here, "..", "RNS", "__init__.py"))
            if os.path.exists(candidate): return os.path.dirname(os.path.dirname(candidate))
            for entry in [os.getcwd()] + list(sys.path):
                if not entry: continue
                candidate = os.path.join(entry, "RNS", "__init__.py")
                if os.path.exists(candidate): return os.path.dirname(candidate)
            raise RuntimeError("CRNS could not locate the RNS source tree")

        def _all_source_modules(self):
            rns_root = os.path.join(self.source_root, "RNS")
            for root, dirs, files in os.walk(rns_root):
                dirs[:] = [d for d in dirs if d not in ("__pycache__", "i2plib")]
                rel = os.path.relpath(root, self.source_root).replace(os.sep, ".")
                for fn in sorted(files):
                    if not fn.endswith(".py"): continue
                    if fn == "__init__.py": fullname = rel
                    else: fullname = rel + "." + fn[:-3]
                    yield fullname

        def _mirror_target(self, fullname, is_package):
            rel = fullname.split(".")
            basename = rel[-1]
            if is_package: return os.path.join(self.mirror_root, *rel, "__init__" + self.abi_suffix)
            return os.path.join(self.mirror_root, *rel[:-1], basename + self.abi_suffix)

        def _source(self, fullname):
            rel = fullname.split(".")
            package_source = os.path.join(self.source_root, *rel, "__init__.py")
            if os.path.exists(package_source): return package_source, True
            module_source = os.path.join(self.source_root, *rel) + ".py"
            if os.path.exists(module_source): return module_source, False
            return None, None

        def _compile(self, fullname, source):
            basename = fullname.rsplit(".", 1)[-1]
            extension = Extension(name=basename, sources=[source])
            extension.cython_directives = COMPILE_DIRECTIVES
            so_path = pyxbuild.pyx_to_dll(source, ext=extension, build_in_temp=True, pyxbuild_dir=self.build_dir)
            abi_suffix = os.path.basename(so_path)[len(basename):]
            return so_path, abi_suffix

        def _is_current(self, fullname, source, is_package):
            target = self._mirror_target(fullname, is_package)
            if not os.path.exists(target): return False
            try: return os.path.getmtime(target) >= os.path.getmtime(source)
            except Exception: return False

        def _ensure_compiled(self, fullname, source, is_package):
            target = self._mirror_target(fullname, is_package)
            if os.path.exists(target) and os.path.getmtime(target) >= os.path.getmtime(source): return target
            so_path, abi_suffix = self._compile(fullname, source)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            os.replace(so_path, target)
            return target

        def precompile(self):
            if not os.path.isdir(self.mirror_root):
                os.makedirs(self.mirror_root, exist_ok=True)

            pending = []
            for fullname in self._all_source_modules():
                source, is_package = self._source(fullname)
                if source is None:
                    continue
                if self._is_current(fullname, source, is_package):
                    self._unmark_buildable(fullname)
                    continue
                if self._is_unbuildable(fullname, source):
                    continue
                
                pending.append((fullname, source, is_package))

            if not pending: return

            jobs = min(_compile_jobs(), len(pending))
            if jobs > 1:
                # Round-robin assignment spreads the large modules evenly
                # across workers.
                chunks = [[] for _ in range(jobs)]
                for i, item in enumerate(pending): chunks[i % jobs].append(item)

                workers_dir = os.path.join(self.build_dir, "workers")
                os.makedirs(workers_dir, exist_ok=True)
                procs = []
                for w in range(jobs):
                    tasks = []
                    for fullname, source, is_package in chunks[w]:
                        tasks.append({ "fullname": fullname,
                                       "basename": fullname.rsplit(".", 1)[-1],
                                       "source": source,
                                       "target": self._mirror_target(fullname, is_package),
                                       "scratch": os.path.join(workers_dir, "w{}".format(w)),
                                       "directives": COMPILE_DIRECTIVES })
                    task_path = os.path.join(workers_dir, "tasks{}.json".format(w))
                    with open(task_path, "w") as fh: json.dump(tasks, fh)
                    procs.append((w, task_path, subprocess.Popen([sys.executable, "-c", _WORKER_SCRIPT, task_path],
                                                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)))

                for w, task_path, proc in procs:
                    stdout, stderr = proc.communicate()
                    if proc.returncode != 0:
                        for fullname, _, _ in chunks[w]:
                            source, _ = self._source(fullname)
                            self._mark_unbuildable(fullname, source)
                            print(f"CRNS: compile worker failed for {fullname}, using interpreted module", file=sys.stderr)
                        if stderr:
                            tail = stderr.decode(errors="replace").strip().splitlines()[-5:]
                            print("CRNS: worker output:\n" + "\n".join(tail), file=sys.stderr)
                        continue
                    
                    results = {}
                    try:
                        with open(task_path + ".results", "r") as fh:
                            for entry in json.load(fh): results[entry["fullname"]] = entry
                    except Exception: results = {}
                    
                    for fullname, _, _ in chunks[w]:
                        entry = results.get(fullname)
                        if entry is not None and entry.get("ok"): self._unmark_buildable(fullname)
                        else:
                            error = "" if entry is None else entry.get("error", "")
                            source, _ = self._source(fullname)
                            self._mark_unbuildable(fullname, source)
                            print(f"Could not compile {fullname}, using interpreted module: {error}", file=sys.stderr)
            else:
                for fullname, source, is_package in pending:
                    try:
                        self._ensure_compiled(fullname, source, is_package)
                        self._unmark_buildable(fullname)
                    except Exception as e:
                        self._mark_unbuildable(fullname, source)
                        print(f"Could not compile {fullname}, using interpreted module: {e}", file=sys.stderr)

        def _spec(self, fullname, source, is_package, compile_attempted):
            if fullname == "RNS.vendor.i2plib" or fullname.startswith("RNS.vendor.i2plib."):
                rel = fullname.split(".")
                locations = [os.path.join(self.source_root, *rel)] if is_package else None
                return spec_from_file_location(fullname, source, loader=SourceFileLoader(fullname, source), submodule_search_locations=locations)

            if compile_attempted:
                try:
                    target = self._ensure_compiled(fullname, source, is_package)
                    self._unmark_buildable(fullname)
                    if is_package: locations = [os.path.dirname(target)]
                    else:          locations = None
                    return spec_from_file_location(fullname, target, loader=ExtensionFileLoader(fullname, target),
                                                   submodule_search_locations=locations)
                except Exception as e:
                    self._mark_unbuildable(fullname, source)
                    print(f"Could not compile {fullname}, using interpreted module: {e}", file=sys.stderr)

            rel = fullname.split(".")
            locations = [os.path.join(self.source_root, *rel)] if is_package else None
            return spec_from_file_location(fullname, source,loader=SourceFileLoader(fullname, source),submodule_search_locations=locations)

        def find_spec(self, fullname, path, target=None):
            if not isinstance(fullname, str):                          return None
            if not (fullname == "RNS" or fullname.startswith("RNS.")): return None

            source, is_package = self._source(fullname)
            if source is None: return None

            return self._spec(fullname, source, is_package, compile_attempted=not self._is_unbuildable(fullname, source))

    _importfinder = _ImportFinder()
    sys.meta_path.insert(0, _importfinder)

def notice_job():
    global noticed
    started = time.time()
    while compiling:
        if time.time() > started+notice_delay and compiling:
            noticed = True
            print("Compiling RNS object code...")
            sys.stdout.flush()
            break
        time.sleep(0.1)

compiling = True
threading.Thread(target=notice_job, daemon=True).start()
_importfinder.precompile()
import RNS; compiling = False
if noticed: print("Compilation done"); sys.stdout.flush()
