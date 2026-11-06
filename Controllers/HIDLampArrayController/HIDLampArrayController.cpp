/*---------------------------------------------------------*\
| HIDLampArrayController.cpp                                |
|                                                           |
|   Driver for HID LampArray Devices                        |
|                                                           |
|   Adam Honse (calcprogrammer1@gmail.com)      26 Mar 2024 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-only                   |
\*---------------------------------------------------------*/

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>
#include "hid_util.h"
#include "HIDLampArrayController.h"
#include "LogManager.h"
#include "StringUtils.h"

HIDLampArrayController::HIDLampArrayController(hid_device *dev_handle, const char *path)
{
    dev                                 = dev_handle;
    location                            = path;
    feature_report_length               = 0;

    /*-----------------------------------------------------*\
    | Parse report IDs from descriptor                      |
    \*-----------------------------------------------------*/
    unsigned int  curr_collection_usage = 0;
    unsigned char data_len              = 0;
    unsigned char key                   = 0;
    unsigned char key_cmd               = 0;
    unsigned char key_size              = 0;
    unsigned int  pos                   = 0;
    unsigned char report_descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
    unsigned char report_id             = 0;
    int           size                  = hid_get_report_descriptor(dev, report_descriptor, sizeof(report_descriptor));
    unsigned int  usage                 = 0;
    unsigned char usage_page            = 0;

    /*-----------------------------------------------------*\
    | Create a list of usages and their report IDs          |
    \*-----------------------------------------------------*/
    std::unordered_map<unsigned int, unsigned int> usage_to_report_id;

    /*-----------------------------------------------------*\
    | Feature report sizes. Report ID, Report Size and      |
    | Report Count are HID global items: they stay in       |
    | effect until changed (End Collection does not reset   |
    | them), so they are tracked separately from report_id  |
    | above, which is only used for the usage lookup.       |
    \*-----------------------------------------------------*/
    std::map<unsigned int, unsigned int> feature_bits_per_report_id;
    unsigned int  global_report_id      = 0;
    unsigned int  global_report_size    = 0;
    unsigned int  global_report_count   = 0;

    /*-----------------------------------------------------*\
    | A failed descriptor read returns -1; treated as a     |
    | length it walks off the end of the buffer.            |
    \*-----------------------------------------------------*/
    LOG_INFO("[HID LampArray DIAG] Report descriptor size: %d", size);

    /*-----------------------------------------------------*\
    | DIAG: dump the descriptor exactly as hidapi returns   |
    | it (on Windows it is reconstructed by hidapi)         |
    \*-----------------------------------------------------*/
    for(int line_start = 0; line_start < size; line_start += 32)
    {
        char line[32 * 3 + 1];
        int  line_pos = 0;

        for(int i = line_start; (i < size) && (i < line_start + 32); i++)
        {
            line_pos += snprintf(&line[line_pos], sizeof(line) - line_pos, "%02X ", report_descriptor[i]);
        }

        LOG_INFO("[HID LampArray DIAG] Desc %04X: %s", line_start, line);
    }

    if(size < 0)
    {
        LampArray.LampCount     = 0;
        LampArray.LampArrayKind = HID_LAMPARRAY_KIND_UNDEFINED;
        return;
    }

    while(pos < (unsigned int)size)
    {
        get_hid_item_size(report_descriptor, size, pos, &data_len, &key_size);

        key                             = report_descriptor[pos];
        key_cmd                         = key & 0xFC;

        switch(key)
        {
            case 0xA1:
                curr_collection_usage   = usage;
                if(usage_page == 0x59 && report_id)
                {
                    usage_to_report_id[curr_collection_usage] = report_id;
                }
                break;

            case 0xC0:
                curr_collection_usage   = 0;
                report_id               = 0;
                break;
        }

        switch(key_cmd)
        {
            case 0x4:
                usage_page              = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                break;

            case 0x8:
                usage                   = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                if(data_len == 4)
                {
                    int local_usage_page = usage >> 16;
                    usage &= 0x0000FFFF;
                    if (local_usage_page == 0x59 && report_id)
                    {
                        usage_to_report_id[usage] = report_id;
                    }
                }
                break;

            case 0x84:
                report_id               = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                if (usage_page == 0x59 && curr_collection_usage)
                {
                    usage_to_report_id[curr_collection_usage] = report_id;
                }
                break;
        }
        switch(key_cmd)
        {
            case 0x74:  /* Report Size (global)         */
                global_report_size      = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                break;

            case 0x84:  /* Report ID (global)           */
                global_report_id        = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                break;

            case 0x94:  /* Report Count (global)        */
                global_report_count     = get_hid_report_bytes(report_descriptor, size, data_len, pos);
                break;

            case 0xB0:  /* Feature (main)               */
                feature_bits_per_report_id[global_report_id] += global_report_size * global_report_count;
                break;
        }

        pos += data_len + key_size;
    }

    /*-----------------------------------------------------*\
    | FeatureReportByteLength = largest feature report in   |
    | bytes + 1 byte for the report ID                      |
    \*-----------------------------------------------------*/
    for(const std::pair<const unsigned int, unsigned int>& feature : feature_bits_per_report_id)
    {
        unsigned int length = ((feature.second + 7) / 8) + 1;

        if(length > feature_report_length)
        {
            feature_report_length = length;
        }
    }

    LOG_DEBUG("[HID LampArray] Feature report length from descriptor: %u bytes", feature_report_length);

    /*-----------------------------------------------------*\
    | Get the report IDs for each report                    |
    \*-----------------------------------------------------*/
    for(const std::pair<const unsigned int, unsigned int>& pair : usage_to_report_id)
    {
        switch(pair.first)
        {
            case 0x02:
                ids.LampArrayAttributesReportID     = pair.second;
                break;

            case 0x20:
                ids.LampAttributesRequestReportID   = pair.second;
                break;

            case 0x22:
                ids.LampAttributesResponseReportID  = pair.second;
                break;

            case 0x50:
                ids.LampMultiUpdateReportID         = pair.second;
                break;

            case 0x60:
                ids.LampRangeUpdateReportID         = pair.second;
                break;

            case 0x70:
                ids.LampArrayControlReportID        = pair.second;
                break;
        }
    }

    /*-----------------------------------------------------*\
    | If reports are missing, return                        |
    \*-----------------------------------------------------*/
    bool report_missing                 = (ids.LampArrayAttributesReportID == 0)
                                       || (ids.LampAttributesRequestReportID == 0)
                                       || (ids.LampAttributesResponseReportID == 0)
                                       || (ids.LampMultiUpdateReportID == 0)
                                       || (ids.LampRangeUpdateReportID == 0)
                                       || (ids.LampArrayControlReportID == 0);

    LOG_INFO("[HID LampArray DIAG] Report IDs: Attributes=%d Request=%d Response=%d MultiUpdate=%d RangeUpdate=%d Control=%d, feature length=%u",
             ids.LampArrayAttributesReportID, ids.LampAttributesRequestReportID, ids.LampAttributesResponseReportID,
             ids.LampMultiUpdateReportID, ids.LampRangeUpdateReportID, ids.LampArrayControlReportID, feature_report_length);

    if(report_missing)
    {
        LOG_WARNING("[HID LampArray DIAG] At least one LampArray report ID is missing (0 above), device ignored");
        LampArray.LampCount = 0;
        LampArray.LampArrayKind = HID_LAMPARRAY_KIND_UNDEFINED;
        return;
    }

    /*-----------------------------------------------------*\
    | Get LampArrayAttributesReport                         |
    \*-----------------------------------------------------*/
    GetLampArrayAttributesReport();

    /*-----------------------------------------------------*\
    | Set LampAttributesRequestReport for LampId 0          |
    \*-----------------------------------------------------*/
    LOG_INFO("[HID LampArray DIAG] LampArrayAttributes: LampCount=%u Kind=%u", (unsigned int)LampArray.LampCount, (unsigned int)LampArray.LampArrayKind);

    SetLampAttributesRequestReport(0);

    /*-----------------------------------------------------*\
    | Get LampAttributesResponseReport for each LampId      |
    \*-----------------------------------------------------*/
    for(unsigned int LampId = 0; LampId < LampArray.LampCount; LampId++)
    {
        GetLampAttributesResponseReport();
    }
}

HIDLampArrayController::~HIDLampArrayController()
{
    hid_close(dev);
}

std::string HIDLampArrayController::GetDeviceLocation()
{
    return("HID: " + location);
}

std::string HIDLampArrayController::GetDeviceName()
{
    wchar_t name_string[128];
    int ret = hid_get_product_string(dev, name_string, 128);

    if(ret != 0)
    {
        return("");
    }

    return(StringUtils::wstring_to_string(name_string));
}

std::string HIDLampArrayController::GetDeviceSerial()
{
    wchar_t serial_string[128];
    int ret = hid_get_serial_number_string(dev, serial_string, 128);

    if(ret != 0)
    {
        return("");
    }

    return(StringUtils::wstring_to_string(serial_string));
}

std::string HIDLampArrayController::GetDeviceVendor()
{
    wchar_t vendor_string[128];
    int ret = hid_get_manufacturer_string(dev, vendor_string, 128);

    if(ret != 0)
    {
        return("");
    }

    return(StringUtils::wstring_to_string(vendor_string));
}

unsigned int HIDLampArrayController::GetLampArrayKind()
{
    return(LampArray.LampArrayKind);
}

unsigned int HIDLampArrayController::GetLampCount()
{
    return(LampArray.LampCount);
}

std::vector<LampAttributes> HIDLampArrayController::GetLamps()
{
    return(Lamps);
}

/*---------------------------------------------------------*\
| Get Feature buffer length: the descriptor's largest       |
| feature report (what Windows requires and what Windows    |
| Dynamic Lighting itself uses), but never smaller than the |
| structure being read.                                     |
\*---------------------------------------------------------*/
std::size_t HIDLampArrayController::GetFeatureBufferLength(std::size_t minimum_length)
{
    return(std::max((std::size_t)feature_report_length, minimum_length));
}

void HIDLampArrayController::GetLampArrayAttributesReport()
{
    std::vector<unsigned char> usb_buf(GetFeatureBufferLength(sizeof(LampArrayAttributes) + 1), 0);

    /*-----------------------------------------------------*\
    | First byte is the report ID                           |
    \*-----------------------------------------------------*/
    usb_buf[0]          = ids.LampArrayAttributesReportID;

    /*-----------------------------------------------------*\
    | Get the report                                        |
    \*-----------------------------------------------------*/
    if(hid_get_feature_report(dev, usb_buf.data(), usb_buf.size()) < 0)
    {
        LOG_WARNING("[HID LampArray] Get LampArrayAttributes report (ID %d, %u bytes) failed", ids.LampArrayAttributesReportID, (unsigned int)usb_buf.size());
    }

    memcpy(&LampArray, &usb_buf[1], sizeof(LampArray));
}

void HIDLampArrayController::GetLampAttributesResponseReport()
{
    std::vector<unsigned char> usb_buf(GetFeatureBufferLength(sizeof(LampAttributes) + 1), 0);
    LampAttributes  attributes;

    /*-----------------------------------------------------*\
    | First byte is the report ID                           |
    \*-----------------------------------------------------*/
    usb_buf[0]      = ids.LampAttributesResponseReportID;

    /*-----------------------------------------------------*\
    | Get the report                                        |
    \*-----------------------------------------------------*/
    if(hid_get_feature_report(dev, usb_buf.data(), usb_buf.size()) < 0)
    {
        LOG_WARNING("[HID LampArray] Get LampAttributesResponse report (ID %d, %u bytes) failed", ids.LampAttributesResponseReportID, (unsigned int)usb_buf.size());
    }

    memcpy(&attributes, &usb_buf[1], sizeof(attributes));

    /*-----------------------------------------------------*\
    | Store the attributes                                  |
    \*-----------------------------------------------------*/
    Lamps.push_back(attributes);
}

void HIDLampArrayController::SetLampArrayControlReport(unsigned char AutonomousMode)
{
    unsigned char   usb_buf[sizeof(LampArrayControl) + 1];

    memset(usb_buf, 0, sizeof(usb_buf));

    /*-----------------------------------------------------*\
    | First byte is the report ID                           |
    \*-----------------------------------------------------*/
    usb_buf[0] = ids.LampArrayControlReportID;

    /*-----------------------------------------------------*\
    | Fill in control data                                  |
    \*-----------------------------------------------------*/
    ((LampArrayControl *)&usb_buf[1])->AutonomousMode = AutonomousMode;

    /*-----------------------------------------------------*\
    | Send the report                                       |
    \*-----------------------------------------------------*/
    hid_send_feature_report(dev, usb_buf, sizeof(usb_buf));
}

void HIDLampArrayController::SetLampAttributesRequestReport(unsigned short LampId)
{
    unsigned char   usb_buf[sizeof(LampAttributesRequest) + 1];

    memset(usb_buf, 0, sizeof(usb_buf));

    /*-----------------------------------------------------*\
    | First byte is the report ID                           |
    \*-----------------------------------------------------*/
    usb_buf[0] = ids.LampAttributesRequestReportID;

    /*-----------------------------------------------------*\
    | Fill in request data                                  |
    \*-----------------------------------------------------*/
    ((LampAttributesRequest *)&usb_buf[1])->LampId = LampId;

    /*-----------------------------------------------------*\
    | Send the report                                       |
    \*-----------------------------------------------------*/
    hid_send_feature_report(dev, usb_buf, sizeof(usb_buf));
}

void HIDLampArrayController::SetLampMultiUpdateReport(unsigned char LampCount, unsigned char LampUpdateFlags, unsigned short * LampIds, LampArrayColor * UpdateColors)
{
    unsigned char   usb_buf[sizeof(LampMultiUpdate) + 1];

    memset(usb_buf, 0, sizeof(usb_buf));

    /*-----------------------------------------------------*\
    | First byte is the report ID                           |
    \*-----------------------------------------------------*/
    usb_buf[0] = ids.LampMultiUpdateReportID;

    /*-----------------------------------------------------*\
    | Fill in multi update data                             |
    \*-----------------------------------------------------*/
    ((LampMultiUpdate *)&usb_buf[1])->LampCount         = LampCount;
    ((LampMultiUpdate *)&usb_buf[1])->LampUpdateFlags   = LampUpdateFlags;

    for(unsigned char LampId = 0; LampId < LampCount; LampId++)
    {
        ((LampMultiUpdate *)&usb_buf[1])->LampIds[LampId] = LampIds[LampId];
        ((LampMultiUpdate *)&usb_buf[1])->UpdateColors[LampId] = UpdateColors[LampId];
    }

    /*-----------------------------------------------------*\
    | Send the report                                       |
    \*-----------------------------------------------------*/
    hid_send_feature_report(dev, usb_buf, sizeof(usb_buf));
}
