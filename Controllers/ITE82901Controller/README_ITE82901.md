# ITE 82901 OpenRGB 驅動說明（ITE SPB driver 版）

## 架構

OpenRGB 透過 ITE 的 SPB peripheral kernel driver 傳送 I2C 封包，不需要 DLL，也不需要 MSFT8000。

| 檔案 | 說明 |
|---|---|
| `ITE82901Controller_Windows.*` | 開啟 `\\.\<driver_name><uid>`，以 IOCTL / WriteFile / ReadFile 傳輸，並送出 82901 封包 `[0x22][pattern]` |
| `RGBController_ITE82901_Windows.*` | OpenRGB UI 模式 |
| `ITE82901ControllerDetect_Windows.cpp` | 從 `OpenRGB.json` 讀取 driver 名稱與 UID 建立裝置 |

與 ITE 參考 DLL 相同的呼叫流程：

1. `CreateFile("\\.\ITE8853_<UID>", GENERIC_READ|GENERIC_WRITE, 0, ..., FILE_FLAG_OVERLAPPED)`
2. `IOCTL_SPBTESTTOOL_OPEN`（0x700）
3. 寫入：`WriteFile`；讀取：`ReadFile`（最多重試 50 次）
4. 結束：`IOCTL_SPBTESTTOOL_CLOSE`（0x701）+ `CloseHandle`

改善的地方：

- 每次傳輸最多等待 1 秒，driver 沒有回應時會取消 I/O，不會讓 OpenRGB 卡住
- 所有傳輸以 mutex 保護，可安全地從多個執行緒呼叫
- 寫入連續失敗後關閉 handle，下次使用時自動重新開啟（例如睡眠喚醒後）

## 系統需求

- ITE SPB peripheral driver 已安裝，且裝置管理員中可以看到對應裝置
- I2C slave address 由 driver 的 ACPI 資源（BIOS 中的 I2cSerialBus）決定，OpenRGB 端不再設定位址

## 設定

第一次執行會寫入 `%APPDATA%\OpenRGB\OpenRGB.json`：

```json
"ITE82901Devices": {
    "devices": [
        {
            "enabled": true,
            "name": "ITE 82901",
            "driver_name": "ITE8853_",
            "uid": 0,
            "probe_read": false
        }
    ]
}
```

- `driver_name` + `uid` 組成裝置路徑，例如 `ITE8853_` + `0` → `\\.\ITE8853_0`
- 舊版的 `bus`、`address`、`speed_khz` 欄位會被忽略
- 找不到 driver 時不會註冊裝置，log 會顯示 `Could not open \\.\...`

## UI 模式對應

| OpenRGB 模式 | 封包 |
|---|---|
| Default（A 組紫 / B 組白） | `22 1C` |
| Off | `22 00` |
| Flashing | `22 02` |
| Double Flashing | `22 03` |
| Breathing | `22 04` |
| Jump | `22 05` |
| Rainbow Cycle | `22 06` |
| Color Cycle | `22 07` |
| Wave | `22 08` |

## 除錯

`OpenRGB.exe --loglevel 6`，搜尋 `[ITE82901]`：

- `Could not open \\.\ITE8853_0 (error 2)`：找不到裝置，driver 未安裝或 `driver_name` / `uid` 錯誤
- `Could not open ... (error 32)`：其他程式（例如 ITE 測試工具）正在使用這個裝置，請先關閉
- `Could not open ... (error 5)`：權限不足，請以系統管理員身分執行
- `IOCTL_SPBTESTTOOL_OPEN failed`：driver 無法開啟 SPB 連線
- `Write failed after 50 attempts`：I2C 傳輸失敗
