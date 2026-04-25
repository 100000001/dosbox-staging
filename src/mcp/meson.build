libmcp_sources = files(
    'mcp_screenshot.cpp',
    'mcp_server.cpp',
    'mcp_tools.cpp',
    'third_party/cpp-mcp/src/mcp_message.cpp',
    'third_party/cpp-mcp/src/mcp_resource.cpp',
    'third_party/cpp-mcp/src/mcp_server.cpp',
    'third_party/cpp-mcp/src/mcp_tool.cpp',
)

# cpp-mcp's own headers refer to its bundled httplib.h / json.hpp via
# unqualified include paths, so its include/ and common/ directories
# must be on the include path for both its own .cpp files and our
# bridge code.
libmcp_incdir = include_directories(
    'third_party/cpp-mcp/include',
    'third_party/cpp-mcp/common',
)

# Match cpp-mcp's CMake defaults; without these its mcp_server.cpp
# fails to compile.
libmcp_cpp_args = [
    '-DMCP_MAX_SESSIONS=100',
    '-DMCP_SESSION_TIMEOUT=600',
]

libmcp = static_library(
    'mcp',
    libmcp_sources,
    include_directories: [incdir, libmcp_incdir],
    cpp_args: libmcp_cpp_args,
    dependencies: [png_dep, sdl2_dep],
)

libmcp_dep = declare_dependency(
    link_with: libmcp,
    include_directories: include_directories('.'),
    compile_args: libmcp_cpp_args,
)

internal_deps += libmcp_dep
