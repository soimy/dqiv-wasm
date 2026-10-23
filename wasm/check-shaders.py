#!/usr/bin/env python3
"""Validate the renderer's shaders against both GLSL profiles it targets.

The eight shaders in native/GpuRenderer.cpp share one source and only the
version line diverges, so this pulls each body out of the C++ source text and
compiles it twice: once as GLSL ES 3.00 (what a wasm build gets) and once as
desktop GLSL 330 core (what Windows gets). Both have to pass.

The bodies are read from the source, not from a copy, so this checks the text
the compiler actually sees.
"""
import os,re,subprocess,sys,tempfile,shutil

ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE=os.path.join(ROOT,"native","GpuRenderer.cpp")
ES_HEADER="#version 300 es\nprecision highp float;\nprecision highp int;\n"
DESKTOP_HEADER="#version 330 core\n"

VALIDATOR=shutil.which("glslangValidator") or shutil.which("glslang")
if not VALIDATOR:
    sys.exit("glslangValidator not found on PATH")

text=open(SOURCE,encoding="utf-8").read()

# Both headers come out of the source, not out of this script, so the check
# cannot drift from what the compiler actually sees.
headers=re.findall(r'#define DQIV_GLSL_HEADER "([^"]*)"',text)
if len(headers)!=2:
    sys.exit(f"expected the two DQIV_GLSL_HEADER definitions in {SOURCE}, found {len(headers)}")
ES_HEADER,DESKTOP_HEADER=[h.encode().decode("unicode_escape") for h in headers]
matches=re.findall(r'const char\* (\w+)=DQIV_GLSL_HEADER R"\((.*?)\)";',text,re.S)
if len(matches)!=9:
    sys.exit(f"expected 9 shaders in {SOURCE}, found {len(matches)}")

names=[m[0] for m in matches]
print(f"found {len(matches)} shaders: {', '.join(names)}\n")

work=tempfile.mkdtemp(prefix="dqiv-shaders-")
failures=[]
for name,body in matches:
    stage="vert" if name.endswith("Vertex") else "frag"
    for profile,header in ((ES_HEADER.split("\n")[0],ES_HEADER),(DESKTOP_HEADER.split("\n")[0],DESKTOP_HEADER)):
        path=os.path.join(work,f"{name}.{stage}")
        with open(path,"w",encoding="utf-8") as handle:
            handle.write(header+body)
        # No profile flag: glslang reads the flavor from the #version line and
        # the stage from the file extension. (-P is "inject preamble text", not
        # "OpenGL ES profile", and sending the file as its argument is what
        # made the first run of this script look broken.)
        result=subprocess.run([VALIDATOR,path],capture_output=True,text=True)
        ok=result.returncode==0
        print(f"  {'ok ' if ok else 'FAIL'} {name:<18} {profile}")
        if not ok:
            failures.append((name,profile,result.stdout+result.stderr))
        if os.path.exists(path):os.remove(path)

if failures:
    print("\n--- failures ---")
    for name,profile,output in failures:
        print(f"\n{name} ({profile})\n{output.strip()}")
    sys.exit(1)
print(f"\nAll {len(matches)} shaders validate as both GLSL ES 3.00 and GLSL 330 core.")
