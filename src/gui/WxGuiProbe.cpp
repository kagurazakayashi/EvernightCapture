// 【GUI · wxWidgets】依赖接线探针（Windows 与 Linux 同一份源码）
//
// 自有界面已经由同目录的 WxGuiRuntime / WxGuiDialog / WxConsentDriver 实现；这个探针保留下来，
// 只证明依赖发现与链接仍然成立。它不建窗口、不弹框、不参与任何授权决定，也不取一个像素。
//
// 这个程序存在的意义是一条可重复运行的构建事实：wx 的头、宏、编译选项与库四样都真的传给了目标，
// 而且 wxApp 能在当前桌面会话里完成初始化。OnInit 里不建窗口、不弹框，打印完就 return false，
// 所以它既不会截屏，也不会被当成"程序已经能用了"。
//
// 没有图形会话时 wxApp 初始化本身会失败 —— 那属于"隔离图形环境"那一层，不是离线判据。
#include <cstdio>

#include <wx/app.h>
#include <wx/version.h>

class WxGuiProbeApp final : public wxApp {
public:
    bool OnInit() override {
        // wxVERSION_STRING 是宽字符（wxWidgets 的 _T 在两个平台上都给 L""），所以这一格用 %ls；
        // 用 %s 会在 64 位上把指针当 char* 读，打出来是一段乱码或直接崩在这里。
        std::printf("gui-probe: wx %d.%d.%d (%ls) 初始化成功；未建窗口、未弹框、不取像素\n",
                    wxMAJOR_VERSION, wxMINOR_VERSION, wxRELEASE_NUMBER, wxVERSION_STRING);
        return true;
    }

    // 探针不进事件循环：要判的只是"wxApp 能不能在这台机器的图形会话里初始化"，而那件事在
    // OnInit 已经完成。原来这里是 OnInit 里 return false，而 wxGTK 在那条路上把进程退出码
    // 留在 -1（本机实测 255），于是 ./test-all.sh 的 isolated 层会把一次成功读成失败；
    // 改成 OnInit 交回 true + OnRun 直接交回 0，成功就是 0，初始化失败则压根到不了这里。
    int OnRun() override { return 0; }
};

wxIMPLEMENT_APP(WxGuiProbeApp);
