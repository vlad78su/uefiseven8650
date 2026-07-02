/** @file

  Copyright (c) 2020, Seungjoo Kim
  Copyright (c) 2016, Dawid Ciecierski

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "UefiSeven.h"
#include "Display.h"
#include "Util.h"
#include "Filesystem.h"
#include "Int10hHandler.h"
#include "Version.h"
#include "AmdVbios.h"


/**
  -----------------------------------------------------------------------------
  Variables.
  -----------------------------------------------------------------------------
**/


DISPLAY_INFO                mDisplayInfo;
EFI_HANDLE                  mUefiSevenImage       = NULL;
EFI_LOADED_IMAGE_PROTOCOL   *mUefiSevenImageInfo  = NULL;
BOOLEAN                     mVerboseMode          = FALSE;
BOOLEAN                     mSkipErrors           = FALSE;
BOOLEAN                     mForceFakeVesa        = FALSE;
BOOLEAN                     mLogToFile            = FALSE;
CHAR16                      *mEfiFilePath         = NULL;
EFI_FILE_HANDLE             mVolumeRoot           = NULL;
EFI_FILE_HANDLE             mLogFileHandle        = NULL;


/**
  Fills in VESA-compatible information about supported video modes
  in the space left for this purpose at the beginning of the
  generated VGA ROM assembly code.
  (See VESA BIOS EXTENSION Core Functions Standard v3.0, p26+.)

  @param[in] StartAddress Where to begin writing VESA information.
  @param[in] EndAddress   Pointer to the next byte after the end
                          of all video mode information data.

  @retval EFI_SUCCESS     The operation was successful
  @return other           The operation failed.

**/
EFI_STATUS
ShimVesaInformation (
  IN  EFI_PHYSICAL_ADDRESS  StartAddress,
  OUT EFI_PHYSICAL_ADDRESS  *EndAddress
  )
{
  VBE_INFO              *VbeInfoFull;
  VBE_INFO_BASE         *VbeInfo;
  VBE_MODE_INFO         *VbeModeInfo;
  UINT8                 *BufferPtr;
  UINT32                HorizontalOffsetPx;
  UINT32                VerticalOffsetPx;
  EFI_PHYSICAL_ADDRESS  FrameBufferBaseWithOffset;

  if ((StartAddress == 0) || (EndAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (EnsureDisplayAvailable ())) {
    PrintError (L"No display adapters were found, unable to fill in VESA information\n");
    return EFI_NOT_FOUND;
  }

  VbeInfoFull = (VBE_INFO *)(UINTN)StartAddress;
  VbeInfo   = &VbeInfoFull->Base;
  BufferPtr = VbeInfoFull->Buffer;
  CopyMem (VbeInfo->Signature, "VESA", 4);
  VbeInfo->VesaVersion                  = 0x0300;
  VbeInfo->OemNameAddress               = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, VENDOR_NAME, sizeof (VENDOR_NAME));
  BufferPtr += sizeof (VENDOR_NAME);
  VbeInfo->Capabilities                 = BIT0;     
  VbeInfo->ModeListAddress              = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  *(UINT16 *)BufferPtr = 0x00F1;   
  BufferPtr += 2;
  *(UINT16 *)BufferPtr = 0xFFFF;   
  BufferPtr += 2;
  VbeInfo->VideoMem64K                  = (UINT16)((mDisplayInfo.FrameBufferSize + 65535) / 65536);
  VbeInfo->OemSoftwareVersion           = 0x0000;
  VbeInfo->VendorNameAddress            = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, VENDOR_NAME, sizeof (VENDOR_NAME));
  BufferPtr += sizeof (VENDOR_NAME);
  VbeInfo->ProductNameAddress           = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, PRODUCT_NAME, sizeof (PRODUCT_NAME));
  BufferPtr += sizeof (PRODUCT_NAME);
  VbeInfo->ProductRevAddress            = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, PRODUCT_REVISION, sizeof (PRODUCT_REVISION));
  BufferPtr += sizeof (PRODUCT_REVISION);

  VbeModeInfo = (VBE_MODE_INFO *)(VbeInfoFull + 1); 
  VbeModeInfo->ModeAttr                 = BIT7 | BIT6 | BIT5 | BIT4 | BIT3 | BIT1 | BIT0;

  // Windows думает, что рисует в 1024x768
  VbeModeInfo->Width                    = 1024;   
  VbeModeInfo->Height                   = 768;    
  VbeModeInfo->CharCellWidth            = 8;      
  VbeModeInfo->CharCellHeight           = 16;     

  // Высчитываем центровку: (1366 - 1024) / 2 = 171 пиксель отступа слева
  HorizontalOffsetPx        = (mDisplayInfo.HorizontalResolution > 1024) ? (mDisplayInfo.HorizontalResolution - 1024) / 2 : 0;
  VerticalOffsetPx          = (mDisplayInfo.VerticalResolution > 768) ? (mDisplayInfo.VerticalResolution - 768) / 2 * mDisplayInfo.PixelsPerScanLine : 0;
  FrameBufferBaseWithOffset = mDisplayInfo.FrameBufferBase
                                + VerticalOffsetPx * 4      
                                + HorizontalOffsetPx * 4;   

  VbeModeInfo->NumBanks                 = 1;      
  VbeModeInfo->BankSizeKB               = 0;      
  VbeModeInfo->LfbAddress               = (UINT32)FrameBufferBaseWithOffset;            
  VbeModeInfo->BytesPerScanLineLinear   = (UINT16)mDisplayInfo.PixelsPerScanLine * 4;   
  VbeModeInfo->NumImagePagesLessOne     = 0;      
  VbeModeInfo->NumImagesLessOneLinear   = 0;      
  VbeModeInfo->WindowPositioningAddress = 0x0;    
  VbeModeInfo->WindowAAttr              = 0x0;    
  VbeModeInfo->WindowBAttr              = 0x0;    
  VbeModeInfo->WindowGranularityKB      = 0x0;    
  VbeModeInfo->WindowSizeKB             = 0x0;    
  VbeModeInfo->WindowAStartSegment      = 0x0;    
  VbeModeInfo->WindowBStartSegment      = 0x0;    

  VbeModeInfo->NumPlanes                = 1;      
  VbeModeInfo->MemoryModel              = 6;      
  VbeModeInfo->DirectColorModeInfo      = BIT1;   
  VbeModeInfo->BitsPerPixel             = 32;     
  VbeModeInfo->BlueMaskSizeLinear       = 8;
  VbeModeInfo->GreenMaskSizeLinear      = 8;
  VbeModeInfo->RedMaskSizeLinear        = 8;
  VbeModeInfo->ReservedMaskSizeLinear   = 8;

  // Возвращаем обязательную для AMD проверку форматов цвета
  if (mDisplayInfo.PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    VbeModeInfo->BlueMaskPosLinear      = 0;      
    VbeModeInfo->GreenMaskPosLinear     = 8;      
    VbeModeInfo->RedMaskPosLinear       = 16;     
    VbeModeInfo->ReservedMaskPosLinear  = 24;     
  } else if (mDisplayInfo.PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
    VbeModeInfo->RedMaskPosLinear       = 0;      
    VbeModeInfo->GreenMaskPosLinear     = 8;      
    VbeModeInfo->BlueMaskPosLinear      = 16;     
    VbeModeInfo->ReservedMaskPosLinear  = 24;     
  } else {
    PrintError (L"Unsupported value of PixelFormat (%d), aborting\n", mDisplayInfo.PixelFormat);
    return EFI_UNSUPPORTED;
  }

  VbeModeInfo->OffScreenAddress         = 0;      
  VbeModeInfo->OffScreenSizeKB          = 0;      
  VbeModeInfo->MaxPixelClockHz          = 0;      
  VbeModeInfo->Vbe3                     = 0x01;   

  *EndAddress = (UINTN)(VbeModeInfo + 1);         

  return EFI_SUCCESS;
}
/**
  Checkes if an Int10h handler is already defined in the
  Interrupt Vector Table (IVT), points to somewhere
  within VGA ROM memory and this memory is not filled
  with protective opcodes.

  @retval TRUE            An Int10h handler was found in IVT.
  @retval FALSE           An Int10h handler was not found in IVT.

**/
BOOLEAN
IsInt10hHandlerDefined (
  VOID
  )
{
  CONST STATIC UINT8    PROTECTIVE_OPCODE_1 = 0xFF;
  CONST STATIC UINT8    PROTECTIVE_OPCODE_2 = 0x00;
  IVT_ENTRY             *Int10hEntry;
  EFI_PHYSICAL_ADDRESS  Int10hHandler;
  UINT8                 Opcode;

  // Fetch 10h entry in IVT.
  Int10hEntry = (IVT_ENTRY *)(UINTN)IVT_ADDRESS + 0x10;
  // Convert handler address from real mode segment address to 32bit physical address.
  Int10hHandler = (Int10hEntry->Segment << 4) + Int10hEntry->Offset;

  if ((Int10hHandler >= VGA_ROM_ADDRESS) && (Int10hHandler < (VGA_ROM_ADDRESS + VGA_ROM_SIZE))) {
    PrintDebug (L"Int10h IVT entry points at location within VGA ROM memory area (%04x:%04x)\n",
      Int10hEntry->Segment, Int10hEntry->Offset);

    Opcode = *((UINT8 *)Int10hHandler);
    if ((Opcode == PROTECTIVE_OPCODE_1) || (Opcode == PROTECTIVE_OPCODE_2)) {
      PrintDebug (L"First Int10h handler instruction at %04x:%04x (%02x) not valid, rejecting handler\n",
        Int10hEntry->Segment, Int10hEntry->Offset, Opcode);
      return FALSE;
    } else {
      PrintDebug (L"First Int10h handler instruction at %04x:%04x (%02x) valid, accepting handler\n",
        Int10hEntry->Segment, Int10hEntry->Offset, Opcode);
      return TRUE;
    }
  } else {
    PrintDebug (L"Int10h IVT entry points at location (%04x:%04x) outside VGA ROM memory area (%04x..%04x), rejecting handler\n",
      Int10hEntry->Segment, Int10hEntry->Offset, VGA_ROM_ADDRESS, VGA_ROM_ADDRESS+VGA_ROM_SIZE);
    return FALSE;
  }
}


/**
  Attempts to either unlock a memory area for writing or
  lock it to prevent writes. Makes use of a number of approaches
  to achieve the desired result.

  @param[in] StartAddress   Where the desired memory area begins.
  @param[in] Length         Number of bytes from StartAddress that
                            need to be locked or unlocked.
  @param[in] Operation      Whether the area is to be locked or unlocked.

  @retval TRUE              An Int10h handler was found in IVT.
  @retval FALSE             An Int10h handler was not found in IVT.

**/
EFI_STATUS
EnsureMemoryLock (
  IN  EFI_PHYSICAL_ADDRESS    StartAddress,
  IN  UINT32                  Length,
  IN  MEMORY_LOCK_OPERATION   Operation
  )
{
  EFI_STATUS                    Status = EFI_NOT_READY;
  UINT32                        Granularity;
  EFI_LEGACY_REGION_PROTOCOL    *LegacyRegion;
  EFI_LEGACY_REGION2_PROTOCOL   *LegacyRegion2;
  CONST CHAR16                  *OperationStr;

  if ((StartAddress == 0) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Operation) {
    case UNLOCK:
      OperationStr = L"unlock";
      break;
    case LOCK:
      OperationStr = L"lock";
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  //
  // Check if we need to perform any operation.
  //
  if ((Operation == UNLOCK) && CanWriteAtAddress (StartAddress)) {
    PrintDebug (L"Memory at %x already %sed\n", StartAddress, OperationStr);
    Status = EFI_SUCCESS;
  } else if ((Operation == LOCK) && !CanWriteAtAddress (StartAddress)) {
    PrintDebug (L"Memory at %x already %sed\n", StartAddress, OperationStr);
    Status = EFI_SUCCESS;
  }

  //
  // Try to lock/unlock with EfiLegacyRegionProtocol.
  //
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiLegacyRegionProtocolGuid, NULL, (VOID **)&LegacyRegion);
    if (!EFI_ERROR (Status)) {
      if (Operation == UNLOCK) {
        /*Status =*/ LegacyRegion->UnLock (LegacyRegion, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
      } else {
        /*Status =*/ LegacyRegion->Lock (LegacyRegion, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
      }

      PrintDebug (L"%s %sing memory at %x with EfiLegacyRegionProtocol\n",
        EFI_ERROR (Status) ? L"Failure" : L"Success",
        OperationStr,
        StartAddress);
    }
  }

  //
  // Try to lock/unlock with EfiLegacyRegion2Protocol.
  //
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiLegacyRegion2ProtocolGuid, NULL, (VOID **)&LegacyRegion2);
    if (!EFI_ERROR (Status)) {
      if (Operation == UNLOCK) {
        /*Status =*/ LegacyRegion2->UnLock (LegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;;
      } else {
        /*Status =*/ LegacyRegion2->Lock (LegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
      }

      PrintDebug (L"%s %sing memory at %x with EfiLegacyRegion2Protocol\n",
        EFI_ERROR (Status) ? L"Failure" : L"Success",
        OperationStr,
        StartAddress);
    }
  }

  //
  // Try to lock/unlock via an MTRR.
  //
  if (EFI_ERROR (Status) && IsMtrrSupported () && (FIXED_MTRR_SIZE >= Length)) {
    if (Operation == UNLOCK) {
      MtrrSetMemoryAttribute (StartAddress, FIXED_MTRR_SIZE, CacheUncacheable);
      Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    } else {
      MtrrSetMemoryAttribute (StartAddress, FIXED_MTRR_SIZE, CacheWriteProtected);
      Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }

    PrintDebug (L"%s %sing memory at %x with MTRRs\n",
      EFI_ERROR (Status) ? L"Failure" : L"Success",
      OperationStr,
      StartAddress);
  }

  //
  // None of the methods worked?
  //
  if (EFI_ERROR (Status)) {
    PrintDebug (L"Unable to find a way to %s memory at %x\n", OperationStr, StartAddress);
  }

  return Status;
}


/**
  Checks if writes are possible in a particular memory area.

  @param[in] Address      The memory location to be checked.

  @retval TRUE            Writes to the specified location are
                          allowed changes are persisted.
  @retval FALSE           Writes to the specified location are
                          not allowed or have no effect.

**/
BOOLEAN
CanWriteAtAddress (
  IN  EFI_PHYSICAL_ADDRESS  Address
  )
{
  BOOLEAN   CanWrite;
  UINT8     *TestPtr;
  UINT8     OldValue;

  TestPtr = (UINT8 *)(Address);
  OldValue = *TestPtr;

  *TestPtr = *TestPtr + 1;
  CanWrite = OldValue != *TestPtr;

  *TestPtr = OldValue;

  return CanWrite;
}


/**
  Displays an animated logo. It has to be stored in a .bmp file
  whose filename (sans extension) has to match the runtime filename
  of UefiSeven. It must also reside in the same folder as UefiSeven.
  The image will be split into rectangular frames whose side
  is assumed to be equal to the shorter side of the image.

  Eg. if you run UefiSeven.efi and have UefiSeven.bmp in the same
  folder, and UefiSeven.bmp is a valid, 24bpp bmp image file of
  size 200x10000, 50 frames will be shown (top to bottom).

  @retval TRUE              Static logo was successfully retrieved
                            and displayed on screen.
  @retval FALSE             Either the required resource was not found
                            or was unable to switch to graphical output.

**/
BOOLEAN
ShowAnimatedLogo (
  VOID
  )
{
  EFI_STATUS  Status;
  CHAR16      *BmpFilePath = NULL;
  UINT8       *BmpFileContents;
  UINTN       BmpFileBytes;
  IMAGE       *WindowsFlag = NULL;

  if (mEfiFilePath == NULL) {
    return FALSE;
  }

  // Check if <MyName>.bmp exists
  Status = ChangeExtension (mEfiFilePath, L"bmp", (VOID **)&BmpFilePath);
  if (EFI_ERROR (Status) || (BmpFilePath == NULL) || !FileExists (mVolumeRoot, BmpFilePath)) {
    if (BmpFilePath != NULL) {
      FreePool (BmpFilePath);
    }
    return FALSE;
  }

  // Read file contents.
  Status = FileRead (mVolumeRoot, BmpFilePath, (VOID **)&BmpFileContents, &BmpFileBytes);
  FreePool (BmpFilePath);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  Status = BmpFileToImage (BmpFileContents, BmpFileBytes, (VOID **)&WindowsFlag);
  FreePool (BmpFileContents);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  // All fine, let's do some drawing.
  SwitchToGraphics (FALSE);
  ClearScreen ();
  AnimateImage (WindowsFlag);

  // Cleanup & return.
  DestroyImage (WindowsFlag);

  return TRUE;
}


VOID
EFIAPI
PrintFuncNameMessage (
  IN CONST  BOOLEAN   IsError,
  IN CONST  CHAR8     *FuncName,
  IN CONST  CHAR16    *FormatString,
  ...
  )
{
  VA_LIST   Marker;
  CHAR16    *Buffer;
  UINTN     BufferSize;
  CHAR8     *AsciiBuffer;
  UINTN     AsciiBufferSize;

  if ((FuncName == NULL) || (FormatString == NULL) || !(IsError || mVerboseMode || mLogToFile)) {
    return;
  }

  //
  // Generate the main message.
  //
  BufferSize = DEBUG_MESSAGE_LENGTH * sizeof (CHAR16);
  Buffer = (CHAR16 *)AllocatePool (BufferSize);
  if (Buffer == NULL) {
    return;
  }
  VA_START (Marker, FormatString);
  UnicodeVSPrint (Buffer, BufferSize, FormatString, Marker);
  VA_END (Marker);

  if (IsError || mVerboseMode) {
    //
    // Switch to text mode if needed.
    //
    SwitchToText (FALSE);

    //
    // Output using apropriate colors.
    //
    gST->ConOut->SetAttribute (gST->ConOut, EFI_DARKGRAY);
    AsciiPrint ("%.10a ", FuncName);
    gST->ConOut->SetAttribute (gST->ConOut, IsError ? EFI_YELLOW : EFI_LIGHTGRAY);
    if ((gST != NULL) && (gST->ConOut != NULL)) {
      gST->ConOut->OutputString (gST->ConOut, Buffer);
    }

    //
    // Cleanup.
    //
    gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY);
  }

  if (mLogToFile) {
    if (mLogFileHandle != NULL) {
      AsciiBufferSize = AsciiStrLen (FuncName) + 2 + StrLen (Buffer) + 1;
      AsciiBuffer = AllocatePool (AsciiBufferSize);
      if (AsciiBuffer != NULL) {
        AsciiSPrint (AsciiBuffer, AsciiBufferSize, "%a: %s", FuncName, Buffer);
        AsciiBufferSize = AsciiStrLen (AsciiBuffer);
        mLogFileHandle->SetPosition (mLogFileHandle, (UINT64)-1);
        mLogFileHandle->Write (mLogFileHandle, &AsciiBufferSize, AsciiBuffer);
        mLogFileHandle->Flush (mLogFileHandle);
        FreePool (AsciiBuffer);
      }
    }
  }

  FreePool (Buffer);
}


VOID
WaitForEnter (
  IN  BOOLEAN   PrintMessage
  )
{
  EFI_INPUT_KEY   Key;
  UINTN           EventIndex;

  if (PrintMessage) {
    PrintDebug (L"Press Enter to continue\n");
  }

  gST->ConIn->Reset (gST->ConIn, FALSE);
  do {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
    gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  } while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN);
}


VOID
WaitForEnterAndStall (
  IN  BOOLEAN   PrintMessage
  )
{
  WaitForEnter (PrintMessage);
  gBS->Stall (1000 * 1000); // 1 second
}


BOOLEAN
ReadConfig (
  VOID
  )
{
  EFI_STATUS  Status;
  CHAR16      *FilePath = NULL;
  UINT8       *FileContents;
  UINTN       FileBytes;
  VOID        *Context;
  UINTN       Num;

  if (mEfiFilePath == NULL) {
    return FALSE;
  }

  //
  // Preferred UefiSeven.ini, instead of bootx64.ini / bootmgfw.ini.
  //
  // Check if <MyName>.ini exists
  //Status = ChangeExtension (mEfiFilePath, L"ini", (VOID **)&FilePath);
  // Check if UefiSeven.ini exists
  Status = GetFilenameInSameDirectory (mEfiFilePath, L"UefiSeven.ini", (VOID **)&FilePath);

  if (EFI_ERROR (Status) || (FilePath == NULL) || !FileExists (mVolumeRoot, FilePath)) {
    if (FilePath != NULL) {
      FreePool (FilePath);
    }
    return FALSE;
  }

  // Read file contents.
  Status = FileRead (mVolumeRoot, FilePath, (VOID **)&FileContents, &FileBytes);
  FreePool (FilePath);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Context = OpenIniFile (FileContents, FileBytes);
  if (Context == NULL) {
    return FALSE;
  }

  //
  // Check if we should skip warnings and prompts
  //
  Status          = GetDecimalUintnFromDataFile (Context, "config", "skiperrors", &Num);
  mSkipErrors     = (!EFI_ERROR (Status) && (Num == 1));

  //
  // Check if we should force fakevesa
  //
  Status          = GetDecimalUintnFromDataFile (Context, "config", "force_fakevesa", &Num);
  mForceFakeVesa  = (!EFI_ERROR (Status) && (Num == 1));

  //
  // Check if we should run in verbose mode
  //
  Status          = GetDecimalUintnFromDataFile (Context, "config", "verbose", &Num);
  mVerboseMode    = (!EFI_ERROR (Status) && (Num == 1));

  //
  // Check if we should log to file
  //
  Status          = GetDecimalUintnFromDataFile (Context, "config", "logfile", &Num);
  mLogToFile      = (!EFI_ERROR (Status) && (Num == 1));

  CloseIniFile (Context);

  FreePool (FileContents);

  return TRUE;
}


/**
  The entry point for the application.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       VGA ROM shim has been installed successfully
                            or it was found not to be required.
  @retval other             Some error occured during execution.

**/
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  IVT_ENTRY               *IvtInt10hHandlerEntry;
  IVT_ENTRY               NewInt10hHandlerEntry;
  EFI_PHYSICAL_ADDRESS    IvtAddress;
  EFI_STATUS              Status;
  EFI_STATUS              IvtAllocationStatus;
  EFI_STATUS              IvtFreeStatus;
  EFI_INPUT_KEY           Key;
  CHAR16                  *LaunchPath = NULL;
  CHAR16                  *LogFilePath = NULL;
  CHAR16                  *VerboseFilePath = NULL;
  CHAR16                  *SkipFilePath = NULL;
  CHAR16                  *FFVFilePath = NULL;
  EFI_FILE_IO_INTERFACE   *Volume;
  EFI_FILE_INFO           *FileInfo;

  //
  // Try freeing IVT memory area in case it has already been allocated.
  //
  IvtFreeStatus = gBS->FreePages (IVT_ADDRESS, 1);

  //
  // Claim real mode IVT memory area before any allocation can
  // grab it. This can be done as the IDT has already been
  // initialized so we can overwrite the IVT.
  //
  IvtAddress = IVT_ADDRESS;
  IvtAllocationStatus = gBS->AllocatePages (AllocateAddress, EfiBootServicesCode, 1, &IvtAddress);

  PrintDebug (L"Force free IVT area result: %r\n", IvtFreeStatus);

  //
  // Initialization.
  //
  mUefiSevenImage = ImageHandle;
  Status = gBS->HandleProtocol (mUefiSevenImage, &gEfiLoadedImageProtocolGuid, (VOID **)&mUefiSevenImageInfo);
  if (EFI_ERROR (Status)) {
    PrintError (L"Unable to locate EFI_LOADED_IMAGE_PROTOCOL, aborting\n");
    goto Exit;
  }

  // Open volume where UefiSeven resides.
  Status = gBS->HandleProtocol (mUefiSevenImageInfo->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Volume);
  if (EFI_ERROR (Status)) {
    PrintError (L"Unable to find simple file system protocol (error: %r)\n", Status);
    goto Exit;
  } else {
    PrintDebug (L"Found simple file system protocol\n");
  }
  Status = Volume->OpenVolume (Volume, &mVolumeRoot);
  if (EFI_ERROR (Status)) {
    PrintError (L"Unable to open volume (error: %r)\n", Status);
    goto Exit;
  }

  mEfiFilePath = PathCleanUpDirectories (ConvertDevicePathToText (mUefiSevenImageInfo->FilePath, FALSE, FALSE));
  if (mEfiFilePath == NULL) {
    PrintError (L"Unable to locate self-path, aborting\n");
    goto Exit;
  }

  //
  // Read <config>.ini, fallback to check existence of old UefiSeven.* files.
  //
  if (!ReadConfig ()) {
    //
    // Check if we should skip warnings and prompts
    //
    Status = GetFilenameInSameDirectory (mEfiFilePath, L"UefiSeven.skiperrors", (VOID **)&SkipFilePath);
    if (!EFI_ERROR (Status)) {
      mSkipErrors = FileExists (mVolumeRoot, SkipFilePath);
      FreePool (SkipFilePath);
    }

    //
    // Check if we should force fakevesa
    //
    Status = GetFilenameInSameDirectory (mEfiFilePath, L"UefiSeven.force_fakevesa", (VOID **)&FFVFilePath);
    if (!EFI_ERROR (Status)) {
      mForceFakeVesa = FileExists (mVolumeRoot, FFVFilePath);
      FreePool (FFVFilePath);
    }

    //
    // Check if we should run in verbose mode
    //
    Status = GetFilenameInSameDirectory (mEfiFilePath, L"UefiSeven.verbose", (VOID **)&VerboseFilePath);
    if (!EFI_ERROR (Status)) {
      mVerboseMode = FileExists (mVolumeRoot, VerboseFilePath);
      FreePool (VerboseFilePath);
    }
  }

  if (mLogToFile) {
    mLogToFile = FALSE;
    Status = GetFilenameInSameDirectory (mEfiFilePath, L"UefiSeven.log", (VOID **)&LogFilePath);
    if (!EFI_ERROR (Status)) {
	  PrintDebug (L"Clearing previous log file\n");
      FileDelete (mVolumeRoot, LogFilePath);

      Status = mVolumeRoot->Open (
                              mVolumeRoot,
                              &mLogFileHandle,
                              LogFilePath,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                              0);
      if (!EFI_ERROR (Status)) {
        FileInfo = GetFileInfo (mLogFileHandle);
        if (FileInfo != NULL) {
          // Re-enable mLogToFile if its not directory.
          if ((FileInfo->Attribute & EFI_FILE_DIRECTORY) == 0) {
            mLogToFile = TRUE;
          }
        }
      }
      FreePool (LogFilePath);
    }
  }

  //
  // Check if we should run in verbose mode ('v' is pressed).
  //
  if (!mVerboseMode) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status) && (Key.UnicodeChar == L'v')) {
      mVerboseMode = TRUE;
    }
  }

  PrintDebug (L"UefiSeven %s\n", VERSION);

  if (mVerboseMode) {
    PrintDebug (L"You are running in verbose mode, press Enter to continue\n");
    WaitForEnter (FALSE);
  }

  //
  // Show animated logo.
  //
  //if (!mVerboseMode) {
  //  ShowAnimatedLogo ();
  //}

//
  // Force native display resolution for Lenovo E545
  //
  SwitchVideoMode (1366, 768); // Инициализируем экран в родном разрешении матрицы
  if (mVerboseMode || mLogToFile) {
    PrintVideoInfo ();
  }

  if (!MatchCurrentResolution (1366, 768)) {
    PrintError (L"Current display does not seem to support changing to 1366x768 resolution\n");
    if (!mSkipErrors) {
      WaitForEnter (FALSE);
    }
    ForceVideoModeHack (1366, 768);
  }

  //
  // If an Int10h handler exists there either is a real
  // VGA ROM in operation or we installed the shim before.
  //
  if (!mForceFakeVesa) {
    if (IsInt10hHandlerDefined ()) {
      PrintDebug (L"Int10h already has a handler, no further action required\n");
      goto Exit;
    }
  } else {
    PrintDebug (L"Overwriting int10h handler with fakevesa...\n");
  }


  //
  // Unlock VGA ROM memory area for writing first.
  //
  Status = EnsureMemoryLock (VGA_ROM_ADDRESS, (UINT32)VGA_ROM_SIZE, UNLOCK);
  if (EFI_ERROR (Status)) {
    PrintError (L"Unable to unlock VGA ROM memory at %04x, aborting\n", VGA_ROM_ADDRESS);
    goto Exit;
  }

  //
  // Clean and load AMD BIOS
  //
  ZeroMem ((VOID *)VGA_ROM_ADDRESS, VGA_ROM_SIZE);
  CopyMem ((VOID *)VGA_ROM_ADDRESS, AMD_VBIOS, sizeof (AMD_VBIOS));

  //
  // Int10h Address
  //
  NewInt10hHandlerEntry.Segment = (UINT16)((UINT32)VGA_ROM_ADDRESS >> 4); // Получится сегмент 0xC000
  NewInt10hHandlerEntry.Offset  = 0x0003; 

  PrintDebug (L"AMD VBIOS injected successfully. Int10h points to (%04x:%04x)\n",
    NewInt10hHandlerEntry.Segment, NewInt10hHandlerEntry.Offset);

  //
  // Lock VGA ROM memory area to prevent further writes.
  //
  Status = EnsureMemoryLock (VGA_ROM_ADDRESS, (UINT32)VGA_ROM_SIZE, LOCK);
  if (EFI_ERROR (Status)) {
    PrintDebug (L"Unable to lock VGA ROM memory at %x but this is not essential\n",
      VGA_ROM_ADDRESS);
  }

  //
  // Try to point the Int10h vector at shim entry point.
  //
  IvtInt10hHandlerEntry = (IVT_ENTRY *)IVT_ADDRESS + 0x10;
  if (!EFI_ERROR (IvtAllocationStatus)) {
    IvtInt10hHandlerEntry->Segment = NewInt10hHandlerEntry.Segment;
    IvtInt10hHandlerEntry->Offset = NewInt10hHandlerEntry.Offset;
    PrintDebug (L"Int10h IVT entry modified to point at %04x:%04x\n",
      IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset);
  } else if (IvtInt10hHandlerEntry->Segment == NewInt10hHandlerEntry.Segment
    && IvtInt10hHandlerEntry->Offset == NewInt10hHandlerEntry.Offset) {
    PrintDebug (L"Int10h IVT entry could not be modified but already pointing at %04x:%04x\n",
      IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset);
  } else {
    PrintError (L"Unable to claim IVT area at %04x (error: %r)\n", IVT_ADDRESS, IvtAllocationStatus);
    PrintError (L"Int10h IVT entry could not be modified and currently poiting\n");
    PrintError (L"at a wrong memory area (%04x:%04x instead of %04x:%04x).\n",
      IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset,
      NewInt10hHandlerEntry.Segment, NewInt10hHandlerEntry.Offset);
    PrintError (L"Press Enter to try to continue.\n");
    if (!mSkipErrors) {
      WaitForEnter (FALSE);
    }
  }

  //
  // Double check if the handler has been installed properly
  //
  if (IsInt10hHandlerDefined ()) {
    PrintDebug (L"Pre-boot Int10h sanity check success\n");
  } else {
    PrintError (L"Pre-boot Int10h sanity check failed\n");
    PrintError (L"Press Enter to continue.\n");
    if (!mSkipErrors) {
      WaitForEnter (FALSE);
    }
  }

  Exit:

  //
  // Check if we can chainload the Windows Boot Manager.
  //
  if (mEfiFilePath != NULL) {
    Status = ChangeExtension (mEfiFilePath, L"original.efi", (VOID **)&LaunchPath);
  } else {
    Status = EFI_NOT_FOUND;
  }
  if (!EFI_ERROR (Status) && FileExists (mVolumeRoot, LaunchPath)) {
    PrintDebug (L"Found Windows Boot Manager at '%s'\n", LaunchPath);
  } else {
    PrintError (L"Could not find Windows Boot Manager at '%s'\n", LaunchPath);
    //PrintError (L"Rename the original bootx64.efi from efi\\boot\\ to bootx64.original.efi\n");
    PrintError (L"Press Enter to continue.\n");
    WaitForEnter (FALSE);
  }

  //
  // Make it possible to enter Windows Boot Manager.
  //
  if (!mVerboseMode) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status) && Key.ScanCode == SCAN_F8) {
      PrintError (L"F8 keypress detected, switching to text mode\n");
      PrintError (L"Press Enter to continue and then immediately press F8 again\n");
      WaitForEnterAndStall (FALSE);
    }
  } else {
    // For debug mode we should also detect F8 and then wait a little
    // to allow user to fill key buffer with F8 in time but this
    // waiting will be done by the Lauch method.
  }

  if (LaunchPath != NULL) {
    Launch (LaunchPath, mVerboseMode ? &WaitForEnterAndStall : NULL);
    FreePool (LaunchPath);
  }

  if (mEfiFilePath != NULL) {
    FreePool (mEfiFilePath);
  }

  if (mLogToFile) {
    if (mLogFileHandle != NULL) {
      mLogFileHandle->Close (mLogFileHandle);
    }
  }

  if (mVolumeRoot != NULL) {
    mVolumeRoot->Close (mVolumeRoot);
  }

  return EFI_SUCCESS;
}
