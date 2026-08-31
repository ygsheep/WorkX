// WorkX 打包 stb_image 的实现单元：仅在此处展开 STB_IMAGE_IMPLEMENTATION。
// 作为独立静态库目标编译（见 src/tui/CMakeLists.txt），并强制包含
// suppress_msvc_warnings.h 豁免宿主工程的 /we 告警策略。
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
