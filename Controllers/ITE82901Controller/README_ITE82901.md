# ITE 82901 OpenRGB 整合說明

## 安裝步驟

1. 把 zip 裡的 `Controllers\ITE82901Controller\` 和 `dependencies\ITEI2CBridge\` 複製到 OpenRGB 原始碼根目錄。**不要**覆蓋 `OpenRGB.pro`。
2. 打開 `OpenRGB.pro`，搜尋 `LedsValve.bin`，在那一行（x86_64 區塊）的下一行加入：

```
    copydata.commands += $(COPY_FILE) \"$$shell_path($$PWD/dependencies/ITEI2CBridge/x64/ITEI2CBridge.dll          )\" \"$$shell_path($$DESTDIR)\" $$escape_expand(\n\t)
```

   （zip 裡的 `OpenRGB.pro.patch` 是同樣的修改。）
3. 用 **MSVC 64-bit** Kit 建置。新版 OpenRGB 的 `.pro` 只在 MSVC 下才會連結 libusb / hidapi / mbedtls 並複製 DLL（`QMAKE_TARGET.arch` 只有 MSVC 會設定），MinGW 無法建置。

檔名帶 `_Windows` 後綴，OpenRGB.pro 會自動只在 Windows 編譯，不需要手動加進 SOURCES。

## 設定

預設值已內建：**bus 0、7-bit slave address 0x68、100 kHz**，裝置會直接出現在 OpenRGB，不需要改任何設定。

第一次執行時，OpenRGB 會把預設值寫進 `%APPDATA%\OpenRGB\OpenRGB.json`：

```json
"ITE82901Devices": {
    "dll": "ITEI2CBridge.dll",
    "devices": [
        {
            "enabled": true,
            "name": "ITE 82901",
            "bus": 0,
            "address": "0x68",
            "speed_khz": 100,
            "probe_read": false
        }
    ]
}
```

若之前跑過舊版而留下 `enabled:false / address "0x00"` 的範本，新版會自動把它升級成上面的預設值。

之後若要修改（改完按 **Rescan Devices**）：

- `bus`：WinRT 列舉到的第幾個 I2C controller（0 起算）
- `address`：7-bit slave address，可寫 `"0x68"` 或十進位 `104`
- `speed_khz`：`100` 或 `400`（DLL 只認 400 = Fast mode）
- `probe_read`：`true` 時偵測階段讀 1 byte 確認 ACK；82901 若不支援讀取請保持 `false`
- `enabled`：`false` 可停用

程式內的預設值定義在 `ITE82901ControllerDetect_Windows.cpp` 的 `ITE82901_DEFAULT_BUS / ITE82901_DEFAULT_ADDR`。

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

指令表沒有顏色/速度/亮度指令，所以這些模式在 UI 上不會出現顏色選擇器。

## 除錯

Settings → General → 開啟 log（或 `OpenRGB.exe --loglevel 6`），搜尋 `[ITE82901]` / `[ITEI2CBridge]`：

- `Failed to load ...ITEI2CBridge.dll`：DLL 沒放在 exe 旁邊
- `initialdll(...) failed`：bus 索引超出範圍，或該 PCH I2C controller 沒有透過 ACPI/rhproxy 開放給 user-mode（WinRT I2C 的前提）
- `Failed to write pattern`：位址錯誤或裝置沒有 ACK
