/**
 * crid_cli.h — USB Serial/JTAG 命令行（调试用）
 *
 * 两块板同时插在电脑上时，用串口命令比来回切 WiFi 热点省事：
 * 每个 COM 口对应一块板，敲命令即可切模式、拍照、看屏幕、查配对码。
 * 只在有 USB 连接时才有实际作用，没有连接也不影响系统运行。
 */

#ifndef CRID_CLI_H
#define CRID_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

void crid_cli_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CRID_CLI_H */
