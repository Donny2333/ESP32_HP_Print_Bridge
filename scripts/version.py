import datetime
import subprocess

Import("env")


def git_output(args, default="unknown"):
    try:
        return subprocess.check_output(args, stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return default


git_rev = git_output(["git", "rev-parse", "--short", "HEAD"])
git_describe = git_output(["git", "describe", "--tags", "--always"], git_rev)
git_status = git_output(["git", "status", "--porcelain"], "")
git_dirty = "dirty" if git_status else "clean"
build_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

if git_dirty == "dirty":
    git_describe = f"{git_describe}-dirty"

env.Append(
    CPPDEFINES=[
        ("FW_VERSION", env.StringifyMacro(git_describe)),
        ("FW_GIT_REV", env.StringifyMacro(git_rev)),
        ("FW_GIT_DIRTY", env.StringifyMacro(git_dirty)),
        ("FW_BUILD_TIME", env.StringifyMacro(build_time)),
    ]
)
