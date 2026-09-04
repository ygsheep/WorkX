// WorkX 构建补丁（强制包含，仅供 ftxui 目标编译时 /FI 注入使用）：
// 宿主工程把 /we4700-4703（MSVC 警告转错误）作为第一方代码的告警策略，
// 但 vendored 第三方 ftxui 的源码会出现 C4702「不可达代码」等告警，
// 被提升为错误后无法编译。此头在 ftxui 每个 TU 的最顶端禁用这几个告警，
// 使其不受宿主 /we 影响，同时不改动 ftxui 源码。
#ifdef _MSC_VER
#pragma warning(disable : 4700 4701 4702 4703)
#endif