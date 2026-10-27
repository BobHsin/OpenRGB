# ITE 82901 OpenRGB 驅動說明（IT8297/IT8237 LED parameter 協定）

## 架構

| 檔案 | 說明 |
|---|---|
| `ITE82901Controller_Windows.*` | 透過 ITE SPB driver（`\\.\<driver_name><uid>`）傳送 IT8297 LED parameter 指令 |
| `RGBController_ITE82901_Windows.*` | OpenRGB UI 模式，轉換成 gLedParameter |
| `ITE82901ControllerDetect_Windows.cpp` | 從 `OpenRGB.json` 讀取設定並建立裝置 |

## 使用的指令

| 指令 | 資料 | 用途 |
|---|---|---|
| `0xE0` | 1 byte（目前 0x00） | Set protocol（每次更新燈效前都會送出；值定義在 `ITE82901_SET_PROTOCOL_DATA`） |
| `0x20+N` | 32 bytes | 寫入 gLedParameter[N] |
| `0x28` | 1 byte（bitN） | 套用 parameter N |

封包格式（預設 I2C 協定）：

```
[0xE0][0x00]
[0x20+N][lZone_Sel x8][00][bMode_Sel][bMax_brightness][bMin_brightness]
[dwColor0 x4][dwColor1 x4][wTime_base0-3 x8][bCtrl_value0-3 x4]
[0x28][1<<N]
```

本驅動不會存檔（不送 `0x5E`），OpenRGB 也不會顯示「Save to device」按鈕；燈效只在 FW 執行期間有效。

FW 斷電或睡眠喚醒後會回到預設的 SMBus 協定，而 SMBus 模式的 FW 收到 I2C 格式的封包時可能不會回報錯誤，
所以每次更新燈效前都會重新送出 `E0 00`。`E0 xx` 在兩種協定下的格式相同，不論 FW 目前在哪個模式都能正確解讀。

多位元組欄位皆為 little-endian，顏色為 0x00RRGGBB。每個指令之間間隔 2 ms（規格要求至少 1 ms）。

## 模式對應

| OpenRGB 模式 | bMode_Sel | 可調整 | 參數 |
|---|---|---|---|
| Off | 1 Static | – | 黑色、亮度 0% |
| Static | 1 Static | 顏色、亮度 | bCtrl_value0 = 0 |
| Breathing | 2 Pulse | 顏色/隨機、速度、亮度 | time0 = time1 = 2000→400 ms，time2 = 200 ms |
| Flashing | 3 Flash | 顏色/隨機、速度、亮度 | on/off 100 ms，cycle 2600→600 ms，閃 1 次 |
| Double Flashing | 3 Flash | 顏色/隨機、速度、亮度 | on/off 100 ms，cycle 2800→800 ms，閃 2 次 |
| Spectrum Cycle | 4 ColorCycle | 速度、亮度 | time0 2400→300 ms，7 色 |
| Meteor | 5 Meteor | 顏色/隨機、速度、亮度 | step 200→20 ms，tail = step × 8 |
| Rainbow Wave | 6 Rainbow | 速度、亮度 | step 300→30 ms，7 色，pulse 關閉 |
| Ripple | 7 Ripple | 2 色/隨機、速度、亮度 | step 200→20 ms，tail = step × 4，cycle 2000 ms |
| Random | 8 Random | 速度、亮度 | off time 100 ms，avg speed 0~2 |
| Wave | 9 Wave2 | 速度、亮度 | speed 500→50 ms，distance 3 |

「隨機」= bCtrl_value0 設為 7，使用晶片內建的 7 色表（指令 0x30）。
速度滑桿 0（最慢）～10（最快），線性換算成上表的毫秒範圍。

## 設定

`%APPDATA%\OpenRGB\OpenRGB.json`（缺少的欄位會自動補上）：

```json
"ITE82901Devices": {
    "devices": [
        {
            "enabled": true,
            "name": "ITE 82901",
            "driver_name": "ITE8853_",
            "uid": 0,
            "led_mask": "0xFFFFFFFFFFFFFFFF",
            "parameter_index": 0,
            "i2c_protocol": true
        }
    ]
}
```

- `driver_name` + `uid`：裝置路徑，例如 `\\.\ITE8853_0`
- `led_mask`：lZone_Sel，bit x = LED x
- `parameter_index`：使用的 gLedParameter 編號（0～7）
- `i2c_protocol`：`true`（預設）= 先送 `E0 00`，封包為 `[cmd][data]`；`false` = 不切換協定，封包為 SMBus block write `[cmd][count][data]`
- 上一版寫入的 `smbus_block` 欄位會自動移除

## 除錯

`OpenRGB.exe --loglevel 6`，搜尋 `[ITE82901]`：

- `Could not open \\.\ITE8853_0 (error 2)`：driver 未安裝或 `driver_name` / `uid` 錯誤
- `Could not open ... (error 32)`：其他程式正在使用這個裝置
- `Failed to set LED parameter`：I2C 傳輸失敗
- `Set protocol (0xE0 0x00) failed`：協定切換指令傳送失敗
- 燈沒有反應但沒有錯誤：確認 FW 支援 `0xE0`，或把 `i2c_protocol` 改成 `false` 改用 SMBus 格式
