// ============================================================================
//  version.h -- 全局配置（唯一配置入口）
//
//  ★ 二次开发时只需要改下面「配置区」里的 5 个值，其余代码一律不用动。
//    改完记得同步这两处的版本数字：
//      - src/payload.rc     FILEVERSION / PRODUCTVERSION / "FileVersion"
//      - iss/cdc_setup.iss  #define AppVer
//    （.rc 和 .iss 不是 C++，用不了这里的宏拼接，只能手写一遍）
// ============================================================================
#ifndef CDCCMD_VERSION_H
#define CDCCMD_VERSION_H

// ---------------------------------------------------------------------------
//  ★ 配置区：改成你自己的信息
// ---------------------------------------------------------------------------

// 你的工具版本号
#define CDCCMD_VERSION      "1.1.0"

// GitHub 用户名或组织名
#define CFG_OWNER           "your-github-name"

// 本工具自己的仓库名（jcgx 自更新会到这里找 release）
#define CFG_REPO            "CDCCMD"

// 「下载主项目」功能的目标仓库名
// （cdc download / cdc releases 拉的就是这个仓库的 release）
#define CFG_MAIN_REPO       "your-main-project"

// 官网 / 项目主页（写进卸载信息与安装包属性）
#define CFG_HOMEPAGE        "https://github.com/your-github-name"

// 发布者署名（写进 exe 属性与安装包属性）
#define CFG_PUBLISHER       "Your Name"

// ---------------------------------------------------------------------------
//  以下由上面推导，一般不需要改
// ---------------------------------------------------------------------------

#ifndef RC_INVOKED

#define CDCCMD_APPNAME      "CDCCMD"

// "owner/repo" 形式，供 GitHub API 用
#define CDCCMD_REPO         CFG_OWNER "/" CFG_REPO
#define CDCCMD_MAIN_REPO    CFG_OWNER "/" CFG_MAIN_REPO

// 文案里显示的「主项目」名字，随便起
#define CDCCMD_MAIN_NAME    "主项目"

#endif // !RC_INVOKED

// rc 也能用的简单字符串宏
#define CDCCMD_PUBLISHER    CFG_PUBLISHER
#define CDCCMD_HOMEPAGE     CFG_HOMEPAGE

#endif // CDCCMD_VERSION_H
