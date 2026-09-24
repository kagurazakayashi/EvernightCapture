// 最小测试窗口程序：EvernightCapture 桌面测试的「自有物件」。
//
// 为什么要自己写一个：系统应用（fontview.exe、notepad.exe）可能把新启动请求转交给
// 既存进程，测试拿到的 PID 随后就消失，只能改用进程名去找窗口 —— 那一找就会找到
// 使用者本来开着的窗口，收尾时更会把它们一并结束。本程序由测试亲手起、亲手记下
// PID 与 HWND，于是「截哪一个」「关哪一个」都只涉及本次建立的对象。
//
// 四种用处：
//   window   被截的目标窗口（签名色 + 16 条颜色带，内容足以判别空帧与交叉污染）
//   solid    单色窗口，用来盖住目标做遮挡对照，或在屏幕左上角摆红块
//   args     把 argv 原样回吐，验证共享进程调用器的引号规则
//   streams  两条流同时大量输出，验证并发读取与二进制不转码
//   hang     先留下可诊断输出再睡死，验证超时只结束本次拥有的进程树
//   pipehold 父进程立刻正常退出，但把标准流句柄留给一条会自己到点结束的孙进程：
//            验证"父进程退了"不等于"两条流都到头了"时，等待仍然有期限
//
// 窗口模式还可以 --block-print-ms N：故意不处理 WM_PRINT，把这条消息所在的线程堵住 N 毫秒，
// 用来造"目标应用的线程卡住了"那种局面 —— 父进程里那次 PrintWindow 于是永不返回，
// 而窗口本身还活着。到点自己醒来，之后的请求照旧能截。
//
// 窗口模式带父进程看门狗与最长存活时间：测试异常退出时不会留下孤儿窗口，
// 也不需要按进程名批量收尾。
//
// 只用 C# 5 语法：编译器是 .NET Framework 自带的 csc.exe。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace EcTestHelper
{
    internal static class Native
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PAINTSTRUCT
        {
            public IntPtr hdc;
            public int fErase;
            public RECT rcPaint;
            public int fRestore;
            public int fIncUpdate;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
            public byte[] rgbRestore;
        }

        // tagMSG 的字段顺序与大小在不同 Windows 版本里变过（尾部多出 dpi 等），而这里从不读它，
        // 只把它原样交回 TranslateMessage / DispatchMessage。于是按 8 个槽位当纯缓冲区用：
        // GetMessageW 想写多少都放得下。照某个版本的字段顺序写死会溢出缓冲区 ——
        // 实测症状是进程以 0xC0000409 直接消失，截图于是拍到一帧全黑。
        [StructLayout(LayoutKind.Sequential)]
        public struct MSG
        {
            public long Slot0;
            public long Slot1;
            public long Slot2;
            public long Slot3;
            public long Slot4;
            public long Slot5;
            public long Slot6;
            public long Slot7;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct WNDCLASSEX
        {
            public uint cbSize;
            public uint style;
            public IntPtr lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            [MarshalAs(UnmanagedType.LPWStr)]
            public string lpszMenuName;
            [MarshalAs(UnmanagedType.LPWStr)]
            public string lpszClassName;
            public IntPtr hIconSm;
        }

        public delegate IntPtr WndProcDelegate(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
        public delegate bool SetDpiContextDelegate(IntPtr context);

        [DllImport("kernel32")]
        public static extern IntPtr GetModuleHandle(string name);

        [DllImport("kernel32", CharSet = CharSet.Ansi)]
        public static extern IntPtr GetProcAddress(IntPtr module, string procName);

        [DllImport("kernel32")]
        public static extern IntPtr OpenProcess(uint access, bool inherit, int processId);

        [DllImport("kernel32")]
        public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32")]
        public static extern bool CloseHandle(IntPtr handle);

        [DllImport("user32", CharSet = CharSet.Unicode)]
        public static extern ushort RegisterClassExW(ref WNDCLASSEX lpwcx);

        [DllImport("user32", CharSet = CharSet.Unicode)]
        public static extern bool UnregisterClassW(string className, IntPtr hModule);

        [DllImport("user32", CharSet = CharSet.Unicode)]
        public static extern IntPtr CreateWindowExW(uint exStyle, string className, string windowText,
            uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu,
            IntPtr instance, IntPtr param);

        [DllImport("user32", CharSet = CharSet.Unicode)]
        public static extern bool SetWindowTextW(IntPtr hWnd, string text);

        [DllImport("user32")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [DllImport("user32")]
        public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

        [DllImport("user32")]
        public static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32")]
        public static extern IntPtr DefWindowProcW(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

        [DllImport("user32")]
        public static extern int GetMessageW(out MSG msg, IntPtr hWnd, uint first, uint last);

        [DllImport("user32")]
        public static extern bool TranslateMessage(ref MSG msg);

        [DllImport("user32")]
        public static extern IntPtr DispatchMessageW(ref MSG msg);

        [DllImport("user32")]
        public static extern void PostQuitMessage(int exitCode);

        [DllImport("user32")]
        public static extern bool PostMessageW(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

        [DllImport("user32")]
        public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

        [DllImport("user32")]
        public static extern IntPtr BeginPaint(IntPtr hWnd, out PAINTSTRUCT ps);

        [DllImport("user32")]
        public static extern bool EndPaint(IntPtr hWnd, ref PAINTSTRUCT ps);

        [DllImport("user32")]
        public static extern bool SetProcessDPIAware();

        [DllImport("user32")]
        public static extern bool UpdateWindow(IntPtr hWnd);

        [DllImport("gdi32")]
        public static extern IntPtr GetStockObject(int index);


    }

    internal sealed class Options
    {
        public string Mode = "window";
        public string ClassName = "ecwindow";
        public string Title = "ecwindow";
        public int Left = 120;
        public int Top = 120;
        public int Width = 420;
        public int Height = 300;
        public int Seed;
        public uint Color = 0x00FF0000u;          // --color 是 RRGGBB：默认纯红
        public bool TopMost;
        public int WatchPid;
        public int MaxLifeSeconds = 900;
        public long StdoutBytes;
        public long StderrBytes;
        public string StderrText;
        public int Spawn;
        public int Seconds = 3600;
        public int Windows = 1;           // --windows N：同一进程建几扇自有窗口（%p 撞名要有这个才造得出来）
        public int BlockPrintMs;        // >0：WM_PRINT / WM_PRINTCLIENT 到了就堵在这条消息里
        public bool NoRedirect;         // WS_EX_NOREDIRECTIONBITMAP：没有 DWM 缓存面，取图只能走 WM_PRINT
        public bool Bordered;           // --bordered：WS_OVERLAPPEDWINDOW（标题栏 + 可拖动边框），
                                        // 于是窗口矩形、客户区矩形、DWM 可见边框矩形三者各不相同
        public int StallMs;             // >0：收到 WM_USER+7 之后把这条消息线程堵住这么久
        public int DestroyAfterMs;      // >0：N 毫秒之后把第一扇窗口 DestroyWindow 掉，进程照旧活着
        public bool Minimize;           // 第一扇建好就最小化：结构化窗口查询要判「最小化窗口默认被排除、
                                        // 写 --list=all 才列进来」，而最小化窗口截不到，所以只做这一扇
        public int RenameAfterMs;       // >0：N 毫秒之后把标题换成 --title2 给的那个
        public string Title2;           // 配合 --rename-after-ms：换成这个标题
        public int PayloadStart = -1;     // "--" 之后的第一条：args 模式把它之后全当数据
        public string PidFile;            // 把自己的 PID 写进这个文件：给 cmd 脚本精确收尾用
    }

    internal static class EcWindow
    {
        private const uint CS_HREDRAW_VREDRAW = 0x0003u;
        private const uint WS_POPUP = 0x80000000u;
        private const uint WS_VISIBLE = 0x10000000u;
        private const uint WS_SYSMENU = 0x00080000u;
        // WS_OVERLAPPEDWINDOW = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME
        //                        | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
        // --bordered 用它：有标题栏与可拖动边框，于是"窗口矩形"与"客户区矩形"真的不重合，
        // 而且 DWM 的可见边框矩形（EXTENDED_FRAME_BOUNDS）也比 GetWindowRect 小一圈。
        // 窗口内部裁剪那两条判据（cropRect 的偏移、--client-area 去掉的那圈边框）只能在这种窗口上现场判。
        private const uint WS_OVERLAPPEDWINDOW = 0x00CF0000u;
        private const uint WS_EX_TOPMOST = 0x00000008u;
        private const uint WS_EX_NOREDIRECTIONBITMAP = 0x00200000u;
        private const uint SWP_NOSIZE = 0x0001u;
        private const uint SWP_NOMOVE = 0x0002u;
        private const uint SWP_NOACTIVATE = 0x0010u;
        private const int SW_SHOW = 5;
        private const int SW_SHOWMINIMIZED = 2;
        private const uint WM_DESTROY = 0x0002u;
        private const uint WM_CLOSE = 0x0010u;
        private const uint WM_PAINT = 0x000Fu;
        private const uint WM_PRINT = 0x0317u;
        private const uint WM_PRINTCLIENT = 0x0318u;
        private const uint WM_USER_STALL = 0x0407u;   // WM_USER+7：测试专用"现在把线程堵住"信号
        private const uint WM_USER_DESTROY = 0x0408u; // WM_USER+8：到点了，把第一扇窗口销毁掉
        private const uint WM_USER_RENAME = 0x0409u;  // WM_USER+9：到点了，把标题换成 --title2
        private const int WHITE_BRUSH = 0;
        private const uint SYNCHRONIZE = 0x00100000u;
        private const uint INFINITE = 0xFFFFFFFFu;
        private const int BAND_COUNT = 16;

        private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);

        private static IntPtr g_hwnd = IntPtr.Zero;
        private static readonly List<IntPtr> g_windows = new List<IntPtr>();   // --windows N 时不止一扇
        private static int g_openWindows;                                       // 关到最后一扇才退出消息循环
        private static int g_blockedPrints;   // 被 --block-print-ms 堵过几次（排障时打印用）
        private static bool g_keepAliveAfterDestroy;   // --destroy-after-ms：关完窗口进程还要留着
        private static readonly List<string> g_classes = new List<string>();    // 本次注册过的类名，收尾逐个注销
        private static Native.WndProcDelegate g_wndProc;      // 必须长期持有，否则委托被 GC 后回调会崩
        private static Options g_opt = new Options();

        [STAThread]
        private static int Main(string[] rawArgs)
        {
            Options opt;
            try
            {
                opt = ParseOptions(rawArgs);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("bad options: " + ex.Message);
                return 2;
            }
            g_opt = opt;

            if (opt.Mode == "window" || opt.Mode == "solid") { return RunWindow(opt); }
            if (opt.Mode == "args") { return RunArgs(opt); }
            if (opt.Mode == "streams") { return RunStreams(opt); }
            if (opt.Mode == "hang") { return RunHang(opt); }
            if (opt.Mode == "pipehold") { return RunPipeHold(opt); }
            Console.Error.WriteLine("unknown mode: " + opt.Mode);
            return 2;
        }

        private static string Need(string[] args, ref int index, string key)
        {
            index++;
            if (index >= args.Length) { throw new ArgumentException("missing value for " + key); }
            return args[index];
        }

        private static Options ParseOptions(string[] args)
        {
            Options o = new Options();
            for (int i = 0; i < args.Length; i++)
            {
                string key = args[i];
                if (key == "--")
                {
                    // args 模式的数据从这之后开始，免得把待验证的参数当成选项解析
                    o.PayloadStart = i + 1;
                    break;
                }
                string value;
                switch (key)
                {
                    case "--mode": o.Mode = Need(args, ref i, key); break;
                    case "--class": o.ClassName = Need(args, ref i, key); break;
                    case "--title": o.Title = Need(args, ref i, key); break;
                    case "--seed": o.Seed = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--color": o.Color = uint.Parse(Need(args, ref i, key), NumberStyles.HexNumber, CultureInfo.InvariantCulture); break;
                    case "--topmost": o.TopMost = true; break;
                    case "--bordered": o.Bordered = true; break;
                    case "--minimize": o.Minimize = true; break;
                    case "--watch-pid": o.WatchPid = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--max-life": o.MaxLifeSeconds = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--stdout-bytes": o.StdoutBytes = long.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--stderr-bytes": o.StderrBytes = long.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--stderr-text": o.StderrText = Need(args, ref i, key); break;
                    case "--spawn": o.Spawn = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--seconds": o.Seconds = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture); break;
                    case "--windows":
                        o.Windows = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture);
                        if (o.Windows < 1 || o.Windows > 8) { throw new ArgumentException("--windows wants 1..8"); }
                        break;
                    case "--pid-file": o.PidFile = Need(args, ref i, key); break;
                    case "--no-redirect": o.NoRedirect = true; break;
                    case "--stall-ms":
                        o.StallMs = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture);
                        if (o.StallMs < 0 || o.StallMs > 120000) { throw new ArgumentException("--stall-ms wants 0..120000"); }
                        break;
                    case "--block-print-ms":
                        o.BlockPrintMs = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture);
                        if (o.BlockPrintMs < 0 || o.BlockPrintMs > 120000) { throw new ArgumentException("--block-print-ms wants 0..120000"); }
                        break;
                    // 下面三条是"目标身份复核"那批判据要的现场：窗口在自己活着的时候被销毁、
                    // 标题在选中之后的某个时刻变了。时间到了自己动手，测试不需要去杀任何进程。
                    case "--destroy-after-ms":
                        o.DestroyAfterMs = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture);
                        if (o.DestroyAfterMs < 0 || o.DestroyAfterMs > 600000) { throw new ArgumentException("--destroy-after-ms wants 0..600000"); }
                        break;
                    case "--rename-after-ms":
                        o.RenameAfterMs = int.Parse(Need(args, ref i, key), CultureInfo.InvariantCulture);
                        if (o.RenameAfterMs < 0 || o.RenameAfterMs > 600000) { throw new ArgumentException("--rename-after-ms wants 0..600000"); }
                        break;
                    case "--title2": o.Title2 = Need(args, ref i, key); break;
                    case "--rect":
                        value = Need(args, ref i, key);
                        string[] parts = value.Split(',');
                        if (parts.Length != 4) { throw new ArgumentException("--rect wants L,T,R,B"); }
                        o.Left = int.Parse(parts[0], CultureInfo.InvariantCulture);
                        o.Top = int.Parse(parts[1], CultureInfo.InvariantCulture);
                        o.Width = int.Parse(parts[2], CultureInfo.InvariantCulture) - o.Left;
                        o.Height = int.Parse(parts[3], CultureInfo.InvariantCulture) - o.Top;
                        break;
                    default: throw new ArgumentException("unknown option: " + key);
                }
            }
            if (o.Width < 1) { o.Width = 1; }
            if (o.Height < 1) { o.Height = 1; }
            if (string.IsNullOrEmpty(o.ClassName)) { throw new ArgumentException("--class must not be empty"); }
            return o;
        }

        // ---------------------------------------------------------------- 窗口

        private static int RunWindow(Options opt)
        {
            // 请求 per-monitor v2：测试传进来的矩形于是就是物理像素座标，
            // 遮挡物与目标不会因 DPI 虚拟化而错位。老系统上退化成 SetProcessDPIAware。
            IntPtr user32 = Native.GetModuleHandle("user32.dll");
            IntPtr addr = Native.GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            bool aware = false;
            if (addr != IntPtr.Zero)
            {
                Native.SetDpiContextDelegate setter =
                    Marshal.GetDelegateForFunctionPointer<Native.SetDpiContextDelegate>(addr);
                aware = setter(new IntPtr(-4));      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            }
            if (!aware) { Native.SetProcessDPIAware(); }

            g_wndProc = new Native.WndProcDelegate(WndProc);
            Native.WNDCLASSEX wc = new Native.WNDCLASSEX();
            wc.cbSize = (uint)Marshal.SizeOf(typeof(Native.WNDCLASSEX));
            wc.style = CS_HREDRAW_VREDRAW;
            wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(g_wndProc);
            wc.hInstance = Native.GetModuleHandle(null);
            wc.hbrBackground = Native.GetStockObject(WHITE_BRUSH);   // 类背景刷：擦背景时也有内容，不留黑底

            uint exStyle = opt.TopMost ? WS_EX_TOPMOST : 0u;
            if (opt.NoRedirect)
            {
                // 没有重定向表面：PrintWindow(PW_RENDERFULLCONTENT) 就没有 DWM 缓存面可拷，
                // 只能把 WM_PRINT 发给窗口自己 —— 于是"目标应用卡住"这件事真的会卡住调用方。
                exStyle |= WS_EX_NOREDIRECTIONBITMAP;
            }
            // --bordered：有标题栏与可拖动边框。窗口内部裁剪的三条判据（cropRect 的偏移、
            // --client-area 去掉的那一圈、DWM 可见边框与 GetWindowRect 那 7 像素之差）
            // 只有在"三种矩形各不相同"的窗口上才判得出来，WS_POPUP 那种全都重合。
            uint style = (opt.Bordered ? WS_OVERLAPPEDWINDOW : WS_POPUP | WS_SYSMENU)
                       | WS_VISIBLE;

            // --windows N：同一个进程建好几扇自有窗口（类名 <class>、<class>-2 …，标题全都一样）。
            // 「同一个进程的两个目标」只能这么造，而 %p / %n 的撞名检测非要它不可。
            // 每扇错开 48 像素：叠着也没关系，WGC 取的是各自的画面。
            for (int k = 1; k <= opt.Windows; k++)
            {
                string className = k == 1 ? opt.ClassName : opt.ClassName + "-" + k.ToString(CultureInfo.InvariantCulture);
                wc.lpszClassName = className;
                if (Native.RegisterClassExW(ref wc) == 0)
                {
                    Console.Error.WriteLine("RegisterClassExW failed for " + className);
                    return 3;
                }
                g_classes.Add(className);

                IntPtr hwnd = Native.CreateWindowExW(exStyle, className, opt.Title, style,
                    opt.Left + (k - 1) * 48, opt.Top + (k - 1) * 48, opt.Width, opt.Height,
                    IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
                if (hwnd == IntPtr.Zero)
                {
                    Console.Error.WriteLine("CreateWindowExW failed for " + className);
                    return 4;
                }
                if (k == 1) { g_hwnd = hwnd; }        // 第一扇仍是 g_hwnd：等窗与判据都按主类名认它
                g_windows.Add(hwnd);
                // 第一扇可以最小化（--minimize），其余照旧可见：一次运行就同时有
                // 「可见的命中」与「最小化的命中」，结构化窗口查询的分页与排除判据要好对号。
                Native.ShowWindow(hwnd, (k == 1 && opt.Minimize) ? SW_SHOWMINIMIZED : SW_SHOW);
                if (opt.TopMost)
                {
                    Native.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                // 先进来一次同步绘制：截图可能紧接着开始，而「还没画过」与「画成全黑」
                // 在 PrintWindow 拿到的自绘内容里长得一模一样。
                Native.UpdateWindow(hwnd);
            }
            g_openWindows = g_windows.Count;

            if (!string.IsNullOrEmpty(opt.PidFile))
            {
                // cmd 脚本拿不到它起的进程的 PID（start 不返回），于是用文件传话：
                // 收尾只 taskkill 这一个 PID，绝不按映像名批量结束。
                try { File.WriteAllText(opt.PidFile, Process.GetCurrentProcess().Id.ToString(CultureInfo.InvariantCulture)); }
                catch (Exception ex) { Console.Error.WriteLine("pid file write failed: " + ex.Message); }
            }

            if (opt.WatchPid > 0) { StartWatchdog(opt.WatchPid); }
            if (opt.MaxLifeSeconds > 0) { StartLifeLimit(opt.MaxLifeSeconds); }
            // 到点自己销毁 / 自己改标题。销毁那一扇之后进程照旧活着 ——
            // "窗口没了"与"进程退了"是两件事，身份复核要能各自被单独判。
            g_keepAliveAfterDestroy = opt.DestroyAfterMs > 0;
            if (opt.DestroyAfterMs > 0) { StartDelayedMessage(WM_USER_DESTROY, opt.DestroyAfterMs); }
            if (opt.RenameAfterMs > 0) { StartDelayedMessage(WM_USER_RENAME, opt.RenameAfterMs); }

            // 同步绘制已经在建窗口时就地做过，这里不再补一次：多扇窗口时那一句只画得到第一扇。
            Native.MSG msg;
            int got;
            while ((got = Native.GetMessageW(out msg, IntPtr.Zero, 0, 0)) != 0)
            {
                if (got == -1) { break; }
                Native.TranslateMessage(ref msg);
                Native.DispatchMessageW(ref msg);
            }
            foreach (string className in g_classes)
            {
                Native.UnregisterClassW(className, wc.hInstance);
            }
            return 0;
        }

        private static IntPtr WndProc(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam)
        {
            if (message == WM_PAINT)
            {
                Native.PAINTSTRUCT ps;
                IntPtr hdc = Native.BeginPaint(hWnd, out ps);
                try { Paint(hWnd, hdc); }
                finally { Native.EndPaint(hWnd, ref ps); }
                return IntPtr.Zero;
            }
            if (message == WM_USER_RENAME)
            {
                // 选中之后的标题刷新：身份复核判的是"当初那条标题条件还成立吗"，
                // 所以这里既能造"换了标题但条件仍成立"，也能造"条件不再成立"。
                if (g_opt.Title2 != null) { Native.SetWindowTextW(hWnd, g_opt.Title2); }
                return IntPtr.Zero;
            }
            if (message == WM_USER_DESTROY)
            {
                // 目标在本次请求被销毁的现场：只关第一扇，进程照旧活着 ——
                // 于是"窗口没了"这件事与"进程退了、PID 被回收"是两件事，各自能被单独判。
                Native.DestroyWindow(hWnd);
                return IntPtr.Zero;
            }
            if (message == WM_USER_STALL)
            {
                // "目标应用的这条消息线程不处理任何事"的现场：坐在 WM_USER+7 里不返回。
                // 于是别人给它发消息都要等它 —— 枚举窗口时的 WM_GETTEXT 是必然要等的，
                // 而 Win10 之后 PrintWindow(PW_RENDERFULLCONTENT) 走 DWM 缓存面、压根不给
                // 窗口发 WM_PRINT（实测如此），所以卡住跨进程调用的判据用这条消息来做。
                // 到点自己醒：窗口没被杀，之后的请求仍然截得到。
                if (g_opt.StallMs > 0) { Thread.Sleep(g_opt.StallMs); }
                return IntPtr.Zero;
            }
            if (message == WM_PRINT || message == WM_PRINTCLIENT)
            {
                // 故意坐在这条消息里不返回：PrintWindow 是把绘制请求发给目标窗口的线程并同步等它画完，
                // 于是调用方那一次调用会一直卡住。这就是"超时参数在 PrintWindow 上没有中断点"的现场。
                // 到点自己醒：窗口没被杀、也没被结束进程，之后的请求仍然截得到。
                if (g_opt.BlockPrintMs > 0)
                {
                    Interlocked.Increment(ref g_blockedPrints);
                    Thread.Sleep(g_opt.BlockPrintMs);
                }
                return Native.DefWindowProcW(hWnd, message, wParam, lParam);
            }
            if (message == WM_CLOSE) { Native.DestroyWindow(hWnd); return IntPtr.Zero; }
            if (message == WM_DESTROY)
            {
                // 多扇窗口时要关到最后一扇才退消息循环，否则看门狗关第一扇就把剩下的留在原地。
                // --destroy-after-ms 是故意只关一扇、进程要留着当"目标已销毁而 PID 还在"的现场，
                // 所以那种情况下最后一扇关了也不 PostQuitMessage（收尾靠看门狗与 --max-life）。
                if (Interlocked.Decrement(ref g_openWindows) <= 0 && !g_keepAliveAfterDestroy)
                {
                    Native.PostQuitMessage(0);
                }
                return IntPtr.Zero;
            }
            return Native.DefWindowProcW(hWnd, message, wParam, lParam);
        }

        private static void Paint(IntPtr hWnd, IntPtr hdc)
        {
            Native.RECT client;
            Native.GetClientRect(hWnd, out client);
            int w = client.Right - client.Left;
            int h = client.Bottom - client.Top;

            // 用 GDI+ 画，不手写 gdi32 的 P/Invoke：FillRect 这类符号在 Win10 之后挪进了
            // gdi32full.dll，DllImport("gdi32") 会撞到 EntryPointNotFoundException，
            // 而这条异常在辅助进程里没人接，进程直接消失（截图于是拍到全黑）。
            using (System.Drawing.Graphics g = System.Drawing.Graphics.FromHdc(hdc, hWnd))
            {
                System.Drawing.Color signature = g_opt.Mode == "solid"
                    ? ColorOf(g_opt.Color)
                    : SignatureColor(g_opt.Seed);
                FillArea(g, 0, 0, w, h, signature);
                if (g_opt.Mode == "solid") { return; }

                // 下半部画 16 条互不相同的颜色：颜色种数是「这一帧真有内容」的判据，
                // 也是并发两轮互相污染时唯一看得见的差异。
                int bandHeight = Math.Max(1, (h / 2) / BAND_COUNT);
                for (int i = 0; i < BAND_COUNT; i++)
                {
                    FillArea(g, 0, h / 2 + i * bandHeight, w, h / 2 + (i + 1) * bandHeight, BandColor(i));
                }

                using (System.Drawing.Font font = new System.Drawing.Font(System.Drawing.FontFamily.GenericSansSerif,
                           14f, System.Drawing.FontStyle.Bold))
                using (System.Drawing.Brush brush = new System.Drawing.SolidBrush(System.Drawing.Color.Black))
                using (System.Drawing.StringFormat format = new System.Drawing.StringFormat())
                {
                    format.Alignment = System.Drawing.StringAlignment.Center;
                    format.LineAlignment = System.Drawing.StringAlignment.Center;
                    g.DrawString(g_opt.Title, font, brush,
                        new System.Drawing.RectangleF(0, 0, w, h / 2), format);
                }
            }
        }

        private static void FillArea(System.Drawing.Graphics g, int left, int top, int right, int bottom,
                                     System.Drawing.Color color)
        {
            using (System.Drawing.SolidBrush brush = new System.Drawing.SolidBrush(color))
            {
                g.FillRectangle(brush, left, top, right - left, bottom - top);
            }
        }

        private static System.Drawing.Color ColorOf(uint rgb)
        {
            // --color 写成 RRGGBB：与 GDI 的 0x00BBGGRR 反着，测试里只用到红，别搞混
            return System.Drawing.Color.FromArgb((int)((rgb >> 16) & 0xFFu),
                                                 (int)((rgb >> 8) & 0xFFu),
                                                 (int)(rgb & 0xFFu));
        }

        // 签名色：G/B 刻意留在 120 以上，永远不会被「纯红」判据误认成遮挡物。
        // harness.psm1 里的 Get-EcSignatureRgb 是同一套算式的另一份实现，改这里要同时改那里。
        private static System.Drawing.Color SignatureColor(int seed)
        {
            int r = 40 + (seed * 53) % 180;
            int g = 120 + (seed * 97) % 110;
            int b = 130 + (seed * 149) % 110;
            return System.Drawing.Color.FromArgb(r, g, b);
        }

        private static System.Drawing.Color BandColor(int index)
        {
            int r = (index * 16 + 8) % 256;
            int g = (index * 41 + 96) % 256;
            int b = (index * 97 + 32) % 256;
            if (r > 190 && g < 70 && b < 70) { g = 96; }    // 别撞上遮挡物的红色判据
            return System.Drawing.Color.FromArgb(r, g, b);
        }

        // 到点把一条消息发给第一扇窗口（销毁 / 改标题）。必须走 PostMessage 到 UI 线程：
        // DestroyWindow 只能由创建它的那条线程调，而这条延时线程是后台线程。
        // 窗口那时已经不归本进程管（或被别的判据关掉）时 PostMessage 只是失败，不做任何补救。
        private static void StartDelayedMessage(uint message, int milliseconds)
        {
            Thread thread = new Thread(delegate()
            {
                Thread.Sleep(milliseconds);
                Native.PostMessageW(g_hwnd, message, IntPtr.Zero, IntPtr.Zero);
            });
            thread.IsBackground = true;
            thread.Start();
        }

        private static void StartWatchdog(int parentPid)
        {
            Thread thread = new Thread(delegate()
            {
                IntPtr handle = Native.OpenProcess(SYNCHRONIZE, false, parentPid);
                if (handle == IntPtr.Zero) { return; }
                Native.WaitForSingleObject(handle, INFINITE);
                CloseSelf();
            });
            thread.IsBackground = true;
            thread.Start();
        }

        private static void StartLifeLimit(int seconds)
        {
            Thread thread = new Thread(delegate()
            {
                Thread.Sleep(seconds * 1000);
                CloseSelf();
            });
            thread.IsBackground = true;
            thread.Start();
        }

        private static void CloseSelf()
        {
            // 本次建的窗口一起关：WM_DESTROY 那边走计数，最后一扇才 PostQuitMessage，
            // 否则 --windows N 时只关得掉第一扇，剩下的会留在屏幕上等 max-life。
            foreach (IntPtr hwnd in g_windows)
            {
                Native.PostMessageW(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            }
            if (g_windows.Count == 0) { Environment.Exit(0); }
        }

        // ---------------------------------------------------------------- 假后端

        // argv 往返：每行「序号<TAB>UTF-8 字节数<TAB>参数原文」。用长度前缀而不是分隔符，
        // 空参数、含 TAB / 换行 / 引号的参数才可能原样验证。序号从 1 起，对应 "--" 之后的位置。
        private static int RunArgs(Options opt)
        {
            if (opt.PayloadStart < 0)
            {
                Console.Error.WriteLine("args mode wants a -- separator");
                return 2;
            }
            string[] argv = Environment.GetCommandLineArgs();
            Stream stdout = Console.OpenStandardOutput();
            int first = opt.PayloadStart + 1;        // argv[0] 是exe，Main 的 args 比它偏一位
            int ordinal = 0;
            for (int i = first; i < argv.Length; i++)
            {
                ordinal++;
                byte[] payload = Encoding.UTF8.GetBytes(argv[i]);
                WriteAscii(stdout, ordinal.ToString(CultureInfo.InvariantCulture) + "\t" +
                           payload.Length.ToString(CultureInfo.InvariantCulture) + "\t");
                stdout.Write(payload, 0, payload.Length);
                stdout.Write(new byte[] { (byte)'\n' }, 0, 1);
            }
            WriteAscii(stdout, "END\t" + ordinal.ToString(CultureInfo.InvariantCulture) + "\t\n");
            stdout.Flush();

            if (opt.StderrText != null)
            {
                byte[] text = Encoding.UTF8.GetBytes(opt.StderrText + "\n");
                Stream stderr = Console.OpenStandardError();
                stderr.Write(text, 0, text.Length);
                stderr.Flush();
            }
            return 0;
        }

        // 双流大量输出：两条流都要写到超过管道缓冲，才会暴露「先读完一条再读另一条」的死锁。
        private static int RunStreams(Options opt)
        {
            WritePattern(Console.OpenStandardOutput(), opt.StdoutBytes);
            WritePattern(Console.OpenStandardError(), opt.StderrBytes);
            return 0;
        }

        private static void WritePattern(Stream stream, long total)
        {
            byte[] buffer = new byte[65536];
            long written = 0;
            while (written < total)
            {
                long remaining = total - written;
                int chunk = remaining > buffer.Length ? buffer.Length : (int)remaining;
                for (int i = 0; i < chunk; i++) { buffer[i] = (byte)((written + i) % 251); }
                stream.Write(buffer, 0, chunk);
                written += chunk;
            }
            stream.Flush();
        }

        // 卡死的子进程：先留下可诊断的输出，再睡死。--spawn 起几条同样卡死的孙进程，
        // 用来验证「超时只结束本次拥有的进程树」。
        private static int RunHang(Options opt)
        {
            StringBuilder children = new StringBuilder();
            for (int i = 0; i < opt.Spawn; i++)
            {
                ProcessStartInfo psi = new ProcessStartInfo(CurrentExePath(),
                    "--mode hang --seconds " + opt.Seconds.ToString(CultureInfo.InvariantCulture));
                psi.UseShellExecute = false;
                psi.CreateNoWindow = true;
                Process child = Process.Start(psi);
                children.Append(child.Id.ToString(CultureInfo.InvariantCulture)).Append(' ');
            }
            Console.Out.WriteLine("hang-prepared children=" + children.ToString().Trim());
            Console.Out.Flush();
            Console.Error.WriteLine("hang-prepared pid=" + Process.GetCurrentProcess().Id);
            Console.Error.Flush();
            Thread.Sleep(opt.Seconds * 1000);
            return 0;
        }

        private static string CurrentExePath()
        {
            using (Process self = Process.GetCurrentProcess())
            {
                return self.MainModule.FileName;
            }
        }

        // 子进程持有管道：起一条继承本进程标准流句柄的孙进程（不重定向，所以它拿的是同一头管道），
        // 然后自己立刻正常退出。读的一方如果以为"父进程退了 = 两条流都到头了"，就会在这里一直挂着。
        // 孙进程自己按 --seconds 到点结束；调用方的期限等待也要能把它连同这棵树一起来的。
        // 孙进程的 PID 会写在 stdout 上，便于调用方在判完之后再核对那一条确实收尾了。
        private static int RunPipeHold(Options opt)
        {
            ProcessStartInfo psi = new ProcessStartInfo(CurrentExePath(),
                "--mode hang --seconds " + opt.Seconds.ToString(CultureInfo.InvariantCulture));
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            Process child = Process.Start(psi);
            Console.Out.WriteLine("pipehold-exited child=" + child.Id);
            Console.Out.Flush();
            Console.Error.WriteLine("pipehold-exited pid=" + Process.GetCurrentProcess().Id);
            Console.Error.Flush();
            return 0;
        }

        private static void WriteAscii(Stream stream, string text)
        {
            byte[] bytes = Encoding.ASCII.GetBytes(text);
            stream.Write(bytes, 0, bytes.Length);
        }
    }
}
