// 跨平台文件 / 目录 / 保存 / 历史 / 配置位置的离线判据。
//
// 判的是生产本体：
//   * src/HostPath.h 的平台实现 —— 程序目录、XDG / 便携模式、临时目录、本地时间、路径比较；
//   * src/HostDirectory.h 的平台实现 —— 路径类型与建一层目录（含符号链接）；
//   * src/FileSave.h 的平台实现 —— 同目录唯一临时文件、原子提交、不覆盖、失败清理；
//   * src/OutputPlan.cpp —— 整批输出名规划与平台自己的碰撞判据；
//   * src/HistoryArchive.cpp —— 日期目录、命名、独占提交、主输出同文件跳过；
//   * src/Delivery.cpp —— 主交付成功 / 历史失败的部分成功记账。
//
// 只使用本次自建的临时目录与合成字节，不碰仓库、不碰任何用户历史截图，也不取一个像素。
// POSIX 专属的权限与符号链接现场在对应小节里用条件编译；Windows 上那些条目不会假装跑过。
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../src/CliOptions.h"
#include "../src/Config.h"
#include "../src/Delivery.h"
#include "../src/Deadline.h"
#include "../src/FileSave.h"
#include "../src/HistoryArchive.h"
#include "../src/HostDirectory.h"
#include "../src/HostFile.h"
#include "../src/HostPath.h"
#include "../src/OutputPlan.h"
#include "../src/core/CliTokens.h"
#include "../src/core/TextCoding.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
    std::fflush(stdout);
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

std::wstring W(const char* utf8) {
    return DecodeUtf8(utf8, std::strlen(utf8)).text;
}

std::filesystem::path FsPath(const std::wstring& path) {
    const std::string bytes = EncodeUtf8(path);
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(bytes.data()), bytes.size()));
}

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> values) {
    return std::vector<uint8_t>(values);
}

bool ReadEquals(const std::wstring& path, const std::vector<uint8_t>& expected) {
    const HostFileRead r = ReadWholeFileBounded(path, 1u << 20);
    return r.outcome == ReadOutcome::kOk && r.bytes == expected;
}

std::wstring UniqueRoot() {
    std::wstring temp;
    if (!TempDirectory(&temp)) return std::wstring();
    LocalDateTime now{};
    LocalDateTimeNow(&now);
    return JoinDirAndFile(temp,
                          L"evernightcapture-save-tests-" + std::to_wstring(CurrentProcessId()) +
                              L"-" + std::to_wstring(now.year) + std::to_wstring(now.month) +
                              std::to_wstring(now.day) + std::to_wstring(now.hour) +
                              std::to_wstring(now.minute) + std::to_wstring(now.second) + L"-" +
                              std::to_wstring(
                                  reinterpret_cast<uintptr_t>(&now) & 0xFFFF));
}

size_t TempLeftovers(const std::wstring& dir) {
    size_t n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(FsPath(dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '~') ++n;
    }
    return n;
}

bool HasErrorCode(const CaptureOutcome& outcome, const wchar_t* code) {
    for (const Diagnostic& d : outcome.errors) {
        if (d.code == code) return true;
    }
    return false;
}

CliTokens Cmd(std::vector<std::wstring> args) {
    CliTokens tokens;
    tokens.PushBack(L"ecapture");   // 程序名占第 0 条
    for (std::wstring& a : args) tokens.PushBack(std::move(a));
    return tokens;
}

HistoryInstant FixedClock() {
    HistoryInstant when;
    when.year = 2026;
    when.month = 10;
    when.day = 21;
    when.hour = 12;
    when.minute = 34;
    when.second = 56;
    return when;
}

class RealSink final : public OutputSink {
public:
    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                  Diagnostic* err) override {
        return SaveFileAtomic(path, bytes, overwrite, err);
    }
    bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted,
                   NativeErrorCode* ioError) override {
        if (emitted) *emitted = 0;
        if (ioError) *ioError = 0;
        (void)bytes;
        return false;
    }
};

// ---------------------------------------------------------------------------
// 保存：正常、覆盖、不覆盖、并发、失败清理
// ---------------------------------------------------------------------------
void CheckSave(const std::wstring& root) {
    Section("保存：正常 / 覆盖 / 不覆盖 / 失败清理");
    const std::wstring dir = JoinDirAndFile(root, L"saves");
    Check(EnsureDirectory(dir).outcome == DirectoryOutcome::kCreated, "自建保存目录");

    const std::wstring target = JoinDirAndFile(dir, L"shot.png");
    const std::vector<uint8_t> first = Bytes({1, 2, 3, 4});
    const std::vector<uint8_t> second = Bytes({9, 8, 7});
    Diagnostic err;
    Check(SaveFileAtomic(target, first, true, &err), "第一次原子写成功");
    Check(ReadEquals(target, first), "写出的字节与输入一致");
    Check(TempLeftovers(dir) == 0, "成功后不留下临时文件");

    Check(SaveFileAtomic(target, second, true, &err), "overwrite=true 能替换既有文件");
    Check(ReadEquals(target, second), "替换后内容是新字节");
    Check(TempLeftovers(dir) == 0, "替换后不留下临时文件");

    err = Diagnostic{};
    Check(!SaveFileAtomic(target, first, false, &err), "overwrite=false 拒绝替换");
    Check(err.code == codes::kFileExists, "拒绝替换报 io.file_exists");
    Check(ReadEquals(target, second), "拒绝替换后旧文件一个字节都没动");
    Check(TempLeftovers(dir) == 0, "拒绝替换后临时文件被清掉");

    // 目标目录不存在：如实失败，不去建目录、不去换位置。
    const std::wstring missing = JoinDirAndFile(JoinDirAndFile(dir, L"missing"), L"x.png");
    err = Diagnostic{};
    Check(!SaveFileAtomic(missing, first, true, &err), "父目录不存在时写失败");
    Check(err.code == codes::kWriteFailed, "父目录不存在报 io.write_failed");
    Check(QueryPathKind(JoinDirAndFile(dir, L"missing")) == PathKind::kMissing,
          "失败后没有悄悄建出一个目录");

    // 目标是目录：失败且目录原样存在。
    const std::wstring asDir = JoinDirAndFile(dir, L"as-dir");
    EnsureDirectory(asDir);
    err = Diagnostic{};
    Check(!SaveFileAtomic(asDir, first, true, &err), "目标被目录占着时写失败");
    Check(err.code == codes::kWriteFailed, "目标被目录占着报 io.write_failed");
    Check(QueryPathKind(asDir) == PathKind::kDirectory, "目录原样存在");

    // 并发同名、不许替换：恰好一个成功，其余看到既有文件，且没有临时文件残留。
    const std::wstring race = JoinDirAndFile(dir, L"race.png");
    constexpr int kThreads = 8;
    std::atomic<int> wins{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            Diagnostic local;
            if (SaveFileAtomic(race, first, false, &local)) wins.fetch_add(1);
        });
    }
    for (std::thread& t : threads) t.join();
    Check(wins.load() == 1, "并发同名 + 不许替换时恰好一个写成功");
    Check(ReadEquals(race, first), "并发落地的文件内容是完整的那一份");
    Check(TempLeftovers(dir) == 0, "并发之后没有临时文件残留");

    // 只读预检只解释、不替代真正写入：正常目标报告没有阻碍。
    const std::wstring fresh = JoinDirAndFile(dir, L"fresh.png");
    Check(ProbeWriteTarget(fresh).blocker == WriteBlocker::kNone, "可写目标的预检报告无阻碍");
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// POSIX：权限失败、符号链接、中文路径
// ---------------------------------------------------------------------------
void CheckPosixSave(const std::wstring& root) {
    Section("POSIX：权限 / 符号链接 / 中文路径");

    const std::wstring locked = JoinDirAndFile(root, L"locked");
    EnsureDirectory(locked);
    const std::wstring target = JoinDirAndFile(locked, L"x.png");
    const std::vector<uint8_t> bytes = Bytes({4, 5, 6});
    const std::string lockedNative = EncodeUtf8(locked);
    const bool canDropWrite = ::chmod(lockedNative.c_str(), 0555) == 0;
    if (canDropWrite && ::geteuid() != 0) {
        Diagnostic err;
        Check(ProbeWriteTarget(target).blocker == WriteBlocker::kDenied,
              "只读目录的预检报告 permission_denied");
        Check(!SaveFileAtomic(target, bytes, true, &err), "目录不可写时真正写入失败");
        Check(err.code == codes::kWriteFailed, "不可写报 io.write_failed");
        Check(!err.detail.empty(), "失败诊断带上预检原因（只作解释）");
        ::chmod(lockedNative.c_str(), 0755);   // 恢复本次自建目录，便于清理
        Check(QueryPathKind(target) == PathKind::kMissing, "失败后没有写出目标文件");
    } else {
        // 以 root 运行或 chmod 不成立：不把没造出来的现场算成通过，也不偷偷换一种判据。
        std::printf("  SKIP  权限失败现场（当前用户是 root 或无法调整本次自建目录权限）\n");
        if (canDropWrite) ::chmod(lockedNative.c_str(), 0755);
    }

    // 目标是一个符号链接：不许替换时看到“已存在”；允许替换时替换链接本身，不跟到链接目标。
    const std::wstring outside = JoinDirAndFile(root, L"outside.txt");
    const std::wstring link = JoinDirAndFile(root, L"link.png");
    const std::vector<uint8_t> outsideBytes = Bytes({0xAA, 0xBB});
    Diagnostic err;
    const bool outsideOk = SaveFileAtomic(outside, outsideBytes, true, &err);
    std::error_code ec;
    std::filesystem::create_symlink(FsPath(outside), FsPath(link), ec);
    Check(outsideOk && !ec, "符号链接现场准备好了");
    if (!ec) {
        err = Diagnostic{};
        Check(!SaveFileAtomic(link, bytes, false, &err), "不许替换时不接受一个既有符号链接");
        Check(err.code == codes::kFileExists, "既有符号链接报 io.file_exists");
        Check(QueryPathKind(link) == PathKind::kSymlink, "拒绝之后链接仍然是一个链接");
        Check(ReadEquals(outside, outsideBytes), "拒绝之后链接目标没有被改写");

        err = Diagnostic{};
        Check(SaveFileAtomic(link, bytes, true, &err), "允许替换时提交成功");
        Check(QueryPathKind(link) == PathKind::kFile, "允许替换时替换的是链接本身");
        Check(ReadEquals(link, bytes), "链接位置现在是一份普通文件");
        Check(ReadEquals(outside, outsideBytes), "链接目标没有被跟随写入");
    }

    // 含空格与中文的目录 / 文件名。
    const std::wstring chineseDir = JoinDirAndFile(root, W("中文 目录"));
    Check(EnsureDirectory(chineseDir).outcome == DirectoryOutcome::kCreated, "自建中文目录");
    const std::wstring chineseFile = JoinDirAndFile(chineseDir, W("截图 测试.png"));
    Check(SaveFileAtomic(chineseFile, bytes, true, &err), "中文与空格路径保存成功");
    Check(ReadEquals(chineseFile, bytes), "中文路径读回的字节一致");
    Check(TempLeftovers(chineseDir) == 0, "中文目录里没有临时文件残留");
}
#else
void CheckPosixSave(const std::wstring&) {
    Section("POSIX：权限 / 符号链接 / 中文路径");
    std::printf("  SKIP  这一组是 POSIX 专属现场（Windows 上不假装跑过）\n");
}
#endif

// ---------------------------------------------------------------------------
// 输出名规划：扩展名、批量序号、碰撞与平台文件名规则
// ---------------------------------------------------------------------------
void CheckOutputPlan(const std::wstring& root) {
    Section("输出名规划");
    Options opt;
    opt.format = ImageFormat::kPng;
    opt.output = JoinDirAndFile(root, L"shot_%i");
    std::vector<OutputTarget> targets(2);
    targets[0].name = L"one";
    targets[1].name = L"two";
    std::vector<std::wstring> paths;
    std::vector<Diagnostic> notes;
    Diagnostic err;
    Check(PlanOutputPaths(opt, targets, &paths, &notes, &err), "多目标模板展开成功");
    Check(paths.size() == 2 && paths[0] != paths[1], "两个目标得到两个不同的名字");
    Check(paths[0].size() > 4 && paths[0].substr(paths[0].size() - 4) == L".png",
          "没有扩展名时按格式补上 .png");

    // 同一个 %n 命中两个目标 -> 整批碰撞，一张都不写。
    Options collide;
    collide.format = ImageFormat::kPng;
    collide.output = JoinDirAndFile(root, L"same_%n.png");
    std::vector<OutputTarget> same(2);
    same[0].name = L"Same Title";
    same[1].name = L"Same Title";
    paths.clear();
    err = Diagnostic{};
    Check(!PlanOutputPaths(collide, same, &paths, nullptr, &err), "两个目标展开成同一个名字时整批拒绝");
    Check(err.code == codes::kOutputCollision && paths.empty(), "碰撞报 io.output_collision");
#ifndef _WIN32
    Check(SafeFileNamePart(L"a/b:c*?.png") == L"a_b:c*?.png",
          "POSIX 只替换 '/'，保留其它合法文件名字符");
#else
    Check(SafeFileNamePart(L"a/b:c*?.png") != L"a/b:c*?.png",
          "Windows 按既有非法字符表替换文件名组件");
#endif
}

// ---------------------------------------------------------------------------
// 历史：命名、独占、同文件跳过；主交付成功而历史失败的部分成功
// ---------------------------------------------------------------------------
void CheckHistory(const std::wstring& root) {
    Section("历史归档与部分成功");
    const std::wstring histRoot = JoinDirAndFile(root, L"history-root");
    HistoryRoot hr;
    hr.dir = histRoot;
    hr.usable = true;

    const HistoryInstant when = FixedClock();
    HistoryArchive archive(hr, [] { return FixedClock(); }, L"test0001", 1);
    const std::vector<uint8_t> bytes = Bytes({0x89, 0x50, 0x4E, 0x47});
    HistoryRecord rec;
    Diagnostic detail;
    Check(archive.Archive(JoinDirAndFile(root, L"primary.png"), ImageFormat::kPng, bytes, &rec,
                          &detail),
          "历史副本独占提交成功");
    Check(rec.state == HistoryRecord::State::kSaved, "历史状态为 saved");
    const std::wstring expectedDir = JoinDirAndFile(histRoot, HistoryDirectoryName(when));
    const std::wstring expected =
        JoinDirAndFile(expectedDir, HistoryFileName(when, CurrentProcessId(), L"test0001", 1,
                                                    ImageFormat::kPng));
    Check(SamePath(rec.file, expected), "历史目录与文件名来自同一次本地时间取样");
    Check(ReadEquals(rec.file, bytes), "历史副本用的是同一份编码字节（不读主输出）");

    HistoryRecord rec2;
    Check(archive.Archive(JoinDirAndFile(root, L"primary.png"), ImageFormat::kPng, bytes, &rec2,
                          &detail),
          "同一秒的下一张改用下一个序号");
    Check(!SamePath(rec2.file, rec.file), "已有历史文件没有被覆盖");

    // 主输出名字正好就是这一次要用的归档名 -> skipped，不自己盖自己。
    HistoryArchive archive3(hr, [] { return FixedClock(); }, L"test0001", 3);
    const std::wstring wouldBe =
        JoinDirAndFile(expectedDir, HistoryFileName(when, CurrentProcessId(), L"test0001", 3,
                                                    ImageFormat::kPng));
    HistoryRecord rec3;
    Check(!archive3.Archive(wouldBe, ImageFormat::kPng, bytes, &rec3, &detail),
          "主输出与归档名相同时不写副本");
    Check(rec3.state == HistoryRecord::State::kSkipped && rec3.code == codes::kHistorySameFile,
          "同文件跳过带 history.same_file");

    // 归档根被一个文件占着：如实失败，不换目录。
    const std::wstring blocker = JoinDirAndFile(root, L"history-blocker");
    Check(SaveFileAtomic(blocker, Bytes({1}), true, &detail), "备好占位文件");
    HistoryRoot blocked;
    blocked.dir = blocker;
    blocked.usable = true;
    HistoryArchive blockedArchive(blocked, [] { return FixedClock(); }, L"block0001", 1);
    HistoryRecord blockedRec;
    Check(!blockedArchive.Archive(JoinDirAndFile(root, L"primary2.png"), ImageFormat::kPng, bytes,
                                  &blockedRec, &detail),
          "归档根是文件时副本失败");
    Check(blockedRec.state == HistoryRecord::State::kFailed &&
              blockedRec.code == codes::kHistoryUnavailable,
          "归档根被文件占着报 history.unavailable");
    Check(QueryPathKind(blocker) == PathKind::kFile, "占位文件没有被删改");

    // 主交付成功、历史失败：图与交付事实保留，退出码是部分成功 7。
    const std::wstring primary = JoinDirAndFile(root, L"delivered.png");
    RealSink sink;
    DeliveryRun run;
    run.sink = &sink;
    run.clock = [] { return 1000ull; };
    run.history = &blockedArchive;
    run.format = ImageFormat::kPng;
    PendingImage pending;
    pending.image.file = primary;
    pending.startedClockMs = 0;
    DeliveryTarget target;
    target.file = primary;
    target.tag = L"tag";
    target.backend = L"test";
    CaptureOutcome outcome;
    DeliveryStep step;
    DeliverImage(run, target, std::move(pending), bytes, &outcome, &step);
    Check(step.delivered && step.recorded, "主图交付事实已记账");
    Check(outcome.images.size() == 1 && ReadEquals(primary, bytes), "主图确实在磁盘上");
    Check(outcome.images[0].history.state == HistoryRecord::State::kFailed &&
              HasErrorCode(outcome, codes::kHistoryUnavailable),
          "历史失败单独记在 images[].history 与 errors 里");
    Check(OutcomeExitCode(outcome) == EX_CAPTURE_FAILED, "主输出成功 + 历史失败退出码为 7");

    // 主输出失败：一个图都不入列，历史那一步根本不开始。
    const std::wstring unwritable = JoinDirAndFile(JoinDirAndFile(root, L"absent"), L"x.png");
    const std::wstring freshHist = JoinDirAndFile(root, L"never-created");
    HistoryRoot freshRoot;
    freshRoot.dir = freshHist;
    freshRoot.usable = true;
    HistoryArchive freshArchive(freshRoot, [] { return FixedClock(); }, L"never001", 1);
    DeliveryRun failing;
    failing.sink = &sink;
    failing.clock = [] { return 1000ull; };
    failing.history = &freshArchive;
    failing.format = ImageFormat::kPng;
    PendingImage pending2;
    pending2.image.file = unwritable;
    DeliveryTarget target2;
    target2.file = unwritable;
    CaptureOutcome outcome2;
    DeliveryStep step2;
    DeliverImage(failing, target2, std::move(pending2), bytes, &outcome2, &step2);
    Check(!step2.delivered && outcome2.images.empty(), "主输出失败时没有交付事实");
    Check(HasErrorCode(outcome2, codes::kWriteFailed), "主输出失败记 io.write_failed");
    Check(OutcomeExitCode(outcome2) == EX_IO_FAILED, "一张都没落地时退出码为 8");
    Check(QueryPathKind(freshHist) == PathKind::kMissing, "主输出失败时历史目录一个都没建");
}

// ---------------------------------------------------------------------------
// 配置位置与优先级
// ---------------------------------------------------------------------------
void CheckConfigLocation(const std::wstring& root) {
    Section("配置位置与优先级");
    const std::wstring iniPath = JoinDirAndFile(root, L"ECAPTURE.ini");
    const std::string ini =
        "[config]\nversion = 1\n[history]\nenabled = false\n";
    {
        FILE* f = std::fopen(EncodeUtf8(iniPath).c_str(), "wb");
        if (f) {
            std::fwrite(ini.data(), 1, ini.size(), f);
            std::fclose(f);
        }
    }
    Check(ReadWholeFileBounded(iniPath, 4096).outcome == ReadOutcome::kOk, "自建配置写好了");

    ParseResult fromConfig = ParseCommandLine(
        Cmd({L"--config", iniPath, L"--title", L"no-such-window"}));
    Check(fromConfig.ok && !fromConfig.options.historyEnabled,
          "配置文件里的 history.enabled=false 生效");

    ParseResult cliWins = ParseCommandLine(
        Cmd({L"--config", iniPath, L"--history", L"on", L"--title", L"no-such-window"}));
    Check(cliWins.ok && cliWins.options.historyEnabled, "命令行 --history on 覆盖配置文件");

    ParseResult portable = ParseCommandLine(
        Cmd({L"--portable", L"--no-config", L"--show-config"}));
    Check(portable.ok && portable.options.portable, "--portable 被解析并落到 Options");
    Check(portable.options.configSelection == L"none" && portable.options.configState == L"not_read",
          "--no-config 仍然一条都不读");

#ifndef _WIN32
    // XDG / HOME 只在本进程内临时改写，结束时恢复；判的仍是生产 ResolveAppPaths / DefaultConfigPath。
    const char* oldHome = std::getenv("HOME");
    const char* oldCfg = std::getenv("XDG_CONFIG_HOME");
    const char* oldState = std::getenv("XDG_STATE_HOME");
    std::string savedHome = oldHome ? oldHome : "";
    std::string savedCfg = oldCfg ? oldCfg : "";
    std::string savedState = oldState ? oldState : "";
    const bool hadHome = oldHome != nullptr;
    const bool hadCfg = oldCfg != nullptr;
    const bool hadState = oldState != nullptr;

    const std::string homeBytes = EncodeUtf8(root);
    ::setenv("HOME", homeBytes.c_str(), 1);
    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("XDG_STATE_HOME");
    AppPaths paths;
    Check(ResolveAppPaths(false, &paths), "用户模式能解析出程序与数据目录");
    Check(paths.configDir == JoinDirAndFile(JoinDirAndFile(root, L".config"), L"evernightcapture"),
          "未设置 XDG_CONFIG_HOME 时配置落在 ~/.config/evernightcapture");
    Check(paths.stateDir ==
              JoinDirAndFile(JoinDirAndFile(root, L".local/state"), L"evernightcapture"),
          "未设置 XDG_STATE_HOME 时状态落在 ~/.local/state/evernightcapture");
    Check(QueryPathKind(paths.configDir) == PathKind::kMissing, "解析目录不创建任何目录");
    std::wstring defaultIni = DefaultConfigPath(false);
    Check(defaultIni == JoinDirAndFile(paths.configDir, L"ECAPTURE.ini"),
          "Linux 默认配置位置是用户配置目录里的 ECAPTURE.ini");

    const std::string xdgCfg = EncodeUtf8(JoinDirAndFile(root, L"xdg-config"));
    const std::string xdgState = EncodeUtf8(JoinDirAndFile(root, L"xdg-state"));
    ::setenv("XDG_CONFIG_HOME", xdgCfg.c_str(), 1);
    ::setenv("XDG_STATE_HOME", xdgState.c_str(), 1);
    Check(ResolveAppPaths(false, &paths) &&
              paths.configDir == JoinDirAndFile(JoinDirAndFile(root, L"xdg-config"),
                                                L"evernightcapture") &&
              paths.stateDir == JoinDirAndFile(JoinDirAndFile(root, L"xdg-state"),
                                               L"evernightcapture"),
          "XDG_CONFIG_HOME / XDG_STATE_HOME 被采用");

    ::setenv("XDG_CONFIG_HOME", "relative-not-used", 1);
    Check(ResolveAppPaths(false, &paths) &&
              paths.configDir == JoinDirAndFile(JoinDirAndFile(root, L".config"),
                                                L"evernightcapture"),
          "相对 XDG 变量被忽略，回退到 HOME 下的配置目录");

    Check(ResolveAppPaths(true, &paths) && paths.portable, "便携模式解析成功");
    Check(paths.configDir == paths.exeDir && paths.stateDir == paths.exeDir,
          "便携模式把配置与历史都放回 exe 旁");
    Check(DefaultConfigPath(true) == JoinDirAndFile(paths.exeDir, L"ECAPTURE.ini"),
          "便携模式的默认配置就在 exe 旁");

    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("XDG_STATE_HOME");
    Check(ResolveAppPaths(false, &paths), "重新解析用户模式（历史根用）");
    HistoryRoot userHistory = ResolveHistoryRoot(false);
    Check(userHistory.usable && userHistory.dir == JoinDirAndFile(paths.stateDir, L"history"),
          "用户模式的历史根在用户状态目录的 history/ 下");
    Check(QueryPathKind(paths.stateDir) == PathKind::kMissing, "解析历史根不创建状态目录");
    // 首次运行：~/.local、~/.local/state、evernightcapture、history、日期层要按顺序建出来。
    HistoryArchive userArchive(userHistory, [] { return FixedClock(); }, L"xdg00001", 1);
    HistoryRecord userRec;
    Diagnostic userDetail;
    Check(userArchive.Archive(JoinDirAndFile(root, L"xdg-primary.png"), ImageFormat::kPng,
                              Bytes({7, 7, 7}), &userRec, &userDetail),
          "用户模式首次归档能补齐状态目录树");
    Check(QueryPathKind(paths.stateDir) == PathKind::kDirectory,
          "状态目录 ~/.local/state/evernightcapture 已被建立");
    Check(QueryPathKind(JoinDirAndFile(userHistory.dir, HistoryDirectoryName(FixedClock()))) ==
              PathKind::kDirectory,
          "日期目录在用户状态目录下建好");
    const std::wstring portableRootDir = JoinDirAndFile(paths.exeDir, L"history");
    const PathKind beforePortable = QueryPathKind(portableRootDir);
    HistoryRoot portableHistory = ResolveHistoryRoot(true);
    Check(portableHistory.usable && portableHistory.dir == portableRootDir,
          "便携模式的历史根在 exe 旁的 history/ 下");
    Check(QueryPathKind(portableRootDir) == beforePortable,
          "解析便携历史根不创建也不改动那个位置");

    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("XDG_STATE_HOME");
    if (hadHome) ::setenv("HOME", savedHome.c_str(), 1); else ::unsetenv("HOME");
    if (hadCfg) ::setenv("XDG_CONFIG_HOME", savedCfg.c_str(), 1); else ::unsetenv("XDG_CONFIG_HOME");
    if (hadState) ::setenv("XDG_STATE_HOME", savedState.c_str(), 1); else ::unsetenv("XDG_STATE_HOME");
#else
    AppPaths paths;
    Check(ResolveAppPaths(false, &paths), "Windows 能解析出程序与数据目录");
    Check(paths.configDir == paths.exeDir && paths.stateDir == paths.exeDir,
          "Windows 配置与历史始终跟随实际运行的 exe");
#endif
}

}  // namespace

int main() {
    std::printf("跨平台文件 / 保存 / 历史 / 配置位置判据\n");
    const std::wstring root = UniqueRoot();
    if (root.empty() || EnsureDirectory(root).outcome != DirectoryOutcome::kCreated) {
        std::printf("  FAIL  无法建立本次临时目录（%s）\n", root.empty() ? "临时目录问不出来" : "");
        std::printf("\nsave-paths: 0 条通过，1 条失败\n");
        return 1;
    }
    std::printf("本次临时目录：%s\n", EncodeUtf8(root).c_str());

    CheckSave(root);
    CheckPosixSave(root);
    CheckOutputPlan(root);
    CheckHistory(root);
    CheckConfigLocation(root);

    std::error_code ec;
    std::filesystem::remove_all(FsPath(root), ec);
    if (ec) std::printf("  NOTE  临时目录未能完全清理：%s\n", ec.message().c_str());

    std::printf("\nsave-paths: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
