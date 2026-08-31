// WorkX 构建补丁（强制包含，仅供 stb_image 目标编译时 /FI 注入使用）：
// 宿主工程把 /we4700-4703（MSVC 警告转错误）作为第一方代码的告警策略，
// 但 vendored 第三方 stb_image 内部会出现 4244/4100/4267/4456 等噪音，
// 且含 /we 提升的未初始化类告警。此头在实现 TU 最顶端禁用这些告警，
// 使其不受宿主 /we 影响，同时不改动 stb_image 源码。
#ifdef _MSC_VER
#pragma warning(disable : 4700 4701 4702 4703)
#pragma warning(disable : 4244 4100 4267 4456 4245 4996 4018 4389)
#endif
