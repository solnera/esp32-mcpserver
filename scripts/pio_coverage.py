# PlatformIO only distributes build_flags into the compiler flags; --coverage
# also has to reach the link line so the driver pulls in the profiling runtime.
# Without this the native-cov env fails with undefined __llvm_profile_* symbols.
Import("env")  # noqa: F821

env.Append(LINKFLAGS=["--coverage"])
