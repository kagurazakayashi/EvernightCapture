#pragma once
// 编码那一步的**共用**接口：一帧 BGRA 像素 -> 一种图片格式的文件字节。
//
// 分层归属（跨平台图像处理与编码这一步定下来的）：
//   * 本头文件与 src/Encoder.cpp 是共用层 —— 帧形状、颜色空间、内存与预算这几道判据只写在这一份
//     地方，两个平台的实现都必须先过它才动手，所以"同一帧在 Windows 与 Linux 上会不会一个编得出
//     一个悄悄编不出"这件事不取决于各平台自己记不记得检查。
//   * 真正写字节的那一段在平台层：`platform/windows/WinEncoder.cpp` 用
//     Windows.Graphics.Imaging.BitmapEncoder（与 WGC 同一套依赖，JPEG 质量接口明确；WIC 的
//     编码器属性袋 CLSID 在当前 SDK 里没有公开声明），`platform/linux/PosixEncoder.cpp` 用
//     libpng + libjpeg(+zlib) 与一份自写的 32 位 BMP。两者各自兑现 EncodeFrameNative()。
//   * 格式支持矩阵在 core/CodecCapability.h：AvailableFormats() 与实际请求的答复都从那张表算，
//     两边不许各抄一份清单。
//
// 这一层接收的帧规范（与 core/Frame.h 同源，这里只写编码这件事要用的那几件）：
//   1. **所有权**：像素是 CapturedFrame 里那份 std::vector<uint8_t> 的所有者。编码不借任何
//      HDC / ID3D11Texture2D / PipeWire 缓冲，所以调用方可以在编码之前就销毁原生资源；
//      编码自己产出的字节写进调用方持有的 vector（本函数不接管它，也不留第二份全局缓冲）。
//   2. **像素布局**：BGRA8 —— 内存里从左到右是 B,G,R,A，自上而下，行距 = frame.stride
//      （可以带行末填充；EncodeFrame 会用 PackTight 重排成紧凑行再交给平台编码器，
//      填充那几字节永远不进图片）。中间帧带 8 字节/像素的那一条（FP16 / 10 位包）在这一步
//      之前必须已经被 HdrColor 映射回 8 位，否则按第 4 条拒。
//   3. **尺寸与形状**：宽高与行距与缓冲区是否自洽、是否超过单边与整帧上限，全部由
//      ImageOps.h 的 CheckFrameShape 在**任何分配之前**判完（64 位算乘加，不靠"分配失败抛异常"）。
//      外部交回来的形状声明因此不可能换来一次无界分配。
//   4. **颜色空间与方向**：来源色彩空间记在 frame.sourceColorSpace，映射与否记在 toneMapped。
//      两条路径都只写 8 位 SDR 图片，所以"确凿是 HDR 而没过一遍映射"那一帧在这里就被拒
//      （capture.hdr_unverifiable），不把广色域像素硬按 SDR 解释 —— 那张图看着像正常图，
//      正是不能默认为正确的结果。方向也是同一件事：旋转由取帧那一步在交付前做完
//      （ImageOps.h 的 RotateCropFrame），编码拿到的永远是"用户看到的那个朝向"，
//      图片字节里不再有第二个旋转参数可以去猜。
//   5. **透明度**：帧里的 alpha 不参与任何合成，也不做预乘/去预乘的猜测 —— 取帧那一层交回什么
//      字节就写什么字节。PNG 与 BMP 保留 alpha 那一路（不合成、不丢数），JPEG 没有 alpha 通道，
//      按不透明写出那三个颜色分量。任何一路都不把"这条路径不表示 alpha"说成"这一帧是全不透明"。
//
// 编码与 GUI / 显示服务器无关：两条路径都不初始化窗口、不连 X11 / Wayland、不加载任何
// 图形工具集（判据里有一条在无 DISPLAY 的会话真跑一次编码）。产出的字节同时供主交付与
// 历史留档使用（Delivery.h 那一段拿的是同一份 buffer，不重拍、不重编码）。
//
// dl 是这一次运行的总预算：编码的每一步都可能很长，所以
//   * 开工之前判一次（预算已经花完就不开始，一个字节都不产）；
//   * Windows 那一路在每个 WinRT 异步等待点只按"还剩多少"等，到点请求取消（IAsyncInfo::Cancel）
//     —— 取消是合作式的，组件自己不回应时我们只能留下那条等待自行结束（期限已经报出去了）；
//   * Linux 那一路在逐行喂数据与逐块落缓冲的地方判同一把预算，到点就从库回调里退出并交回
//     capture.timeout，不会"先把整张图编完再看有没有超时"。
//   两条都到不了的地方（库内部那一段不可中断的压缩）照实记进 README，不假装已经强制停下来了。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"                 // ImageFormat / Diagnostic / codes::
#include "Deadline.h"
#include "core/CodecCapability.h"       // CodecPath / CodecSupport（格式支持矩阵）
#include "core/Frame.h"                 // CapturedFrame（跨平台共用形状）

namespace ecapture {

// 编码前置判据的结论。顺序就是检查顺序：形状那一关排在颜色空间之前，因为一张形状已经说不通的帧
// 连"它是哪一种来源"都问不出来；预算排在最后，因为它是"现在不能开工"而不是"这帧不能编"。
enum class EncodeInput : uint32_t {
    kOk = 0,
    kZeroSize,          // 宽或高为 0（enc.size_zero）
    kBadShape,          // CheckFrameShape 的任何一种不合格形状
    kTooWideForCodec,   // 紧凑字节数装不进 32 位长度参数（WinRT 的 SetPixelData / 库的 uint32 计数）
    kHdrNotMapped,      // 来源确凿是 HDR 而这一帧没过 tone mapping —— 不能硬按 SDR 写
    kFormatUnsupported, // 这条编码路径带不回这一格式（见 core/CodecCapability.h）
    kBudgetSpent,       // 开工之前预算已经用尽
};

// 前置判据的唯一一份实现（src/Encoder.cpp）。返回 true 时 *err 不被触碰；返回 false 时 *err
// 已经是完整诊断（稳定码 + 本地化文案 + hint）。两个平台的 EncodeFrameNative 都必须先调用它，
// 所以这条边界不靠各平台"记得检查"。
bool CheckEncodeInput(const CapturedFrame& frame, ImageFormat format, const Deadline& dl,
                      Diagnostic* err);

// 上面那个结论的 ASCII 名（判据与 -v 细节用，不随 --lang 变）。
const char* EncodeInputName(EncodeInput verdict);

// 平台层填编码那一段诊断的唯一入口（实现在 src/Encoder.cpp，与前置判据共用同一份骨架）：
// option 恒为 --format、value 是格式名、stage 恒为 encode，hint 恒带一句"这个构建现在编得出
// 哪几种"（enc.available）；detail 非空时挂在它后面。两个平台因此不会各拼一份提示，
// 而 "enc.failed" 那一条搬进共用层之前写好的字节也逐字节不变。
// hresult 只在真拿得到那一次返回值的那条文路上出现（不填 0）。返回 false 只为让调用点写成
// `return EncodeFailure(...)`；err 为空时同样返回 false，不写谁也不崩。
bool EncodeFailure(Diagnostic* out, const wchar_t* code, std::wstring message,
                   const std::wstring& format, const std::wstring& detail,
                   const std::wstring& hresult, const wchar_t* backend);

// 平台层兑现的那一条：拿已经过完前置判据、行距已经紧凑（tight.size() == width*4*height）
// 的像素写出图片字节。失败时填 *err 并返回 false；成功时 *bytes 非空。
// 这一条不在共用调用点出现（调用方一律走 EncodeFrame），声明放在这里只为让两个平台层
// 与判据都能挂同一个签名 —— 与 CliArgs.h 那两条平台入口同一类做法：一个构建里只用得到自己那条。
bool EncodeFrameNative(const CapturedFrame& frame, const std::vector<uint8_t>& tight,
                       ImageFormat format, int jpegQuality, const Deadline& dl,
                       std::vector<uint8_t>* bytes, Diagnostic* err);

// 把 frame 编码成 fmt 指定的格式，结果写进 bytes。
// 顺序：CheckEncodeInput → PackTight（行末填充不参与）→ EncodeFrameNative → 空产物核对。
// 任何一步不过都不写 bytes（不留半张图），并交回一条诊断。
bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality, const Deadline& dl,
                 std::vector<uint8_t>* bytes, Diagnostic* err);

// 这个构建走的是哪条编码路径（ASCII 机器名由 CodecPathName 给，进 -v 的诊断与判据）。
CodecPath EncoderCurrentPath();

// 这一格式在**这个构建**里编不编得出来。reason 交回一句恒 ASCII 的说明（登记表那一份理由，
// 或"这个构建没链上 libpng/libjpeg"）。共用实现：先查 core/CodecCapability.h 的矩阵，
// 再问平台层"所需的编码库这次真链上了吗"—— 缺库不会折成"本工具不支持这一格式"，
// 也不会反过来拿"实现写了"冒充能力。
bool EncodeFormatSupported(ImageFormat format, const char** reason);

// 平台层对"这一家编码器这次在不在"的回答（Windows 恒为已就绪；Linux 按配置期实测的
// ECAPTURE_HAVE_PNG / ECAPTURE_HAVE_JPEG 回答）。判据用它核对 kLibraryNotLinked 那一档。
bool CodecLibraryLinked(ImageFormat format);

// 本机可编码的格式名（用于错误提示）：由登记表与本次链上的库算出，按 ImageFormat 的顺序拼，
// 短别名见 FormatDisplayAlias。Windows 上的结果与这条选项存在之前逐字节相同。
std::wstring AvailableFormats();

}  // namespace ecapture
