/*
 *  PART 1
 *
 * First, we have to find the Logitech devices connected to the PC.
 * The current code enumerates and prints out the connected Logitech devices.
 *
 *  Build:  cl /W4 stage1_enumerate.c setupapi.lib hid.lib
 *  Run:    stage1_enumerate.exe
 * ============================================================================
 */

// Silences warnings for functions like strcpy, scanf, etc
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>  // Core Win32 types/handles: HANDLE, DWORD, GUID, CreateFileA, etc
#include <hidsdi.h>   // HidD_*, HidP_*
#include <setupapi.h> // SetupDi* device enumeration
#include <stdio.h>
#include <stdlib.h>

// Links the following libraries when compiling
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// USB Vendor ID assigned to Logitech
#define LOGITECH_VID 0x046D
/* Logitech carries its HID++ control protocol on this page, so it marks the right
 * interface (instead of the generic keyboard/mouse collections). */
#define HIDPP_USAGE_PAGE 0xFF00

int main(void)
{
    // Every HID device interface is registered under this GUID
    GUID guid;
    // HidD_GetHidGuid fills 'guid' with the GUID Windows uses to tag all HID device interfaces
    HidD_GetHidGuid(&guid);

    /* Ask Windows for the set of HID interfaces that are present right now. */
    /* SetupDiGetClassDevs returns an HDEVINFO "device information set" handle.
     *   arg1 &guid  : restrict to the HID interface class we just fetched.
     *   arg2 NULL   : no Plug-and-Play enumerator filter.
     *   arg3 NULL   : no owner window (for any UI).
     *   arg4 flags  : DIGCF_PRESENT      -> only devices currently attached;
     *                 DIGCF_DEVICEINTERFACE -> enumerate interfaces, not nodes. */
    HDEVINFO di = SetupDiGetClassDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    // On failure the function returns INVALID_HANDLE_VALUE rather than NULL
    if (di == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "SetupDiGetClassDevs failed (%lu)\n", GetLastError());
        return 1;
    }

    // Count of matching Logitech HID++ interfaces
    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);

    /* Iterate every HID interface index until the enumeration runs out. */
    /* SetupDiEnumDeviceInterfaces walks the set 'di' one interface at a time:
     *   arg2 NULL  : enumerate across the whole set (not a single device)
     *   arg3 &guid : the interface class to match
     *   arg4 i     : zero-based index; increment each pass
     *   arg5 &ifd  : receives this interface's data
     * It returns FALSE when the index is past the end (last error
     * ERROR_NO_MORE_ITEMS), which ends the loop */
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &guid, i, &ifd); i++)
    {
        DWORD need = 0; // Receives the required buffer size in bytes
        /* First call: passing NULL buffer / 0 size makes it fail on purpose but
         * write the needed byte count into 'need' */
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        // If no size came back there is nothing to retrieve
        if (need == 0)
            continue;

        /* Allocate exactly 'need' bytes. The detail struct has a fixed header
         * followed by the device path string that extends past its declared end */
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det) /* Out of memory: skip this interface, keep going. */
            continue;
        /* cbSize must be the size of the FIXED header only (not 'need'). The API
         * uses it to locate the struct version, not the total allocation. */
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL))
        {
            /* Open the interface so we can read its attributes/capabilities.
             * Shared read+write so we don't fight Logi Options+ for it. */
            /* CreateFileA opens the HID device by its path and returns a HANDLE:
             *   access    : GENERIC_READ | GENERIC_WRITE (we only query here).
             *   share     : FILE_SHARE_READ|WRITE lets other apps keep it open.
             *   OPEN_EXISTING : fail if the device isn't there (don't create).
             * Returns INVALID_HANDLE_VALUE on failure. */
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE)
            {
                HIDD_ATTRIBUTES attr; // Receives VendorID/ProductID/etc.
                attr.Size = sizeof(attr);

                PHIDP_PREPARSED_DATA pp = NULL;

                // Keep only Logitech devices and only the HID++ usage page
                if (HidD_GetAttributes(h, &attr) &&  // read VID/PID
                    attr.VendorID == LOGITECH_VID && // must be Logitech
                    HidD_GetPreparsedData(h, &pp))   // get parsed descriptor
                {
                    HIDP_CAPS caps; // Top-level collection capabilities.
                    /* HidP_GetCaps decodes the preparsed data into caps: usage
                     * page/usage and the input/output report byte lengths. It
                     * returns HIDP_STATUS_SUCCESS on success */
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                        caps.UsagePage == HIDPP_USAGE_PAGE)
                    {
                        printf("[%d] %04x:%04x  usage=%04x:%02x  in=%u out=%u\n",
                               found++, attr.VendorID, attr.ProductID,
                               caps.UsagePage, caps.Usage,
                               caps.InputReportByteLength,
                               caps.OutputReportByteLength);
                        printf("     %s\n", det->DevicePath);
                    }
                    HidD_FreePreparsedData(pp);
                }
                CloseHandle(h);
            }
        }
        free(det);
    }

    // Release the whole device information set returned by SetupDiGetClassDevs.
    SetupDiDestroyDeviceInfoList(di);
    printf("\nFound %d Logitech HID++ interface(s).\n", found);

    return 0;
}
