
/// proxy.c
/// SCSI support routines, called from miniport callback functions, normally
/// at DISPATCH_LEVEL. This includes responding to control requests and
/// queueing work items for requests that need to be carried out at
/// PASSIVE_LEVEL.
/// 
/// Copyright (c) 2012-2017, Arsenal Consulting, Inc. (d/b/a Arsenal Recon) <http://www.ArsenalRecon.com>
/// This source code and API are available under the terms of the Affero General Public
/// License v3.
///
/// Please see LICENSE.txt for full license terms, including the availability of
/// proprietary exceptions.
/// Questions, comments, or requests for clarification: http://ArsenalRecon.com/contact/
///

#include "phdskmnt.h"

#include <Ntddcdrm.h>
#include <Ntddmmc.h>

#include "legacycompat.h"

#define INCLUDE_EXTENDED_CD_DVD_EMULATION

#ifdef USE_SCSIPORT
/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiExecuteRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__in pResultType          pResult
)
{
    KdPrint2(("PhDskMnt::ScsiExecuteRaidControllerUnit: pSrb = 0x%p, CDB = 0x%X Path: %x TID: %x Lun: %x\n",
        pSrb, pSrb->Cdb[0], pSrb->PathId, pSrb->TargetId, pSrb->Lun));

    *pResult = ResultDone;

    switch (pSrb->Cdb[0])
    {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_VERIFY:
        ScsiSetSuccess(pSrb, 0);
        break;

    case SCSIOP_INQUIRY:
        ScsiOpInquiryRaidControllerUnit(pHBAExt, pSrb);
        break;

    default:
        KdPrint(("PhDskMnt::ScsiExecuteRaidControllerUnit: Unknown opcode=0x%X\n", (int)pSrb->Cdb[0]));
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;
    }
}

VOID
ScsiOpInquiryRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PINQUIRYDATA          pInqData = (PINQUIRYDATA)pSrb->DataBuffer;// Point to Inquiry buffer.
    PCDB                  pCdb;

    KdPrint2(("PhDskMnt::ScsiOpInquiryRaidControllerUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    if (pSrb->DataTransferLength > 0)
    {
        RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);
        pInqData->DeviceType = ARRAY_CONTROLLER_DEVICE;
    }

    pCdb = (PCDB)pSrb->Cdb;

    if (pCdb->CDB6INQUIRY3.EnableVitalProductData == 1)
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Received VPD request for page 0x%X\n", pCdb->CDB6INQUIRY.PageCode));

        ScsiOpVPDRaidControllerUnit(pHBAExt, pSrb);

        goto done;
    }

    if (pSrb->DataTransferLength == 0)
    {
        pSrb->DataTransferLength = pCdb->CDB6INQUIRY3.AllocationLength;
    }

    if (pSrb->DataTransferLength > 0)
    {
        RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);
        pInqData->DeviceType = ARRAY_CONTROLLER_DEVICE;
    }

    if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, VendorId))
    {
        pInqData->RemovableMedia = FALSE;
        pInqData->CommandQueue = TRUE;

        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, VendorId) + sizeof(pInqData->VendorId))
            RtlMoveMemory(pInqData->VendorId, pHBAExt->VendorId, 8);

        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, ProductId) + sizeof(pInqData->ProductId))
            RtlMoveMemory(pInqData->ProductId, pHBAExt->ProductId, 16);

        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, ProductRevisionLevel) + sizeof(pInqData->ProductRevisionLevel))
            RtlMoveMemory(pInqData->ProductRevisionLevel, pHBAExt->ProductRevision, 4);
    }

    ScsiSetSuccess(pSrb, sizeof(INQUIRYDATA));

done:
    KdPrint2(("PhDskMnt::ScsiOpInquiry: End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpInquiry.
#endif

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiExecute(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__in pResultType          pResult,
__in PKIRQL               LowestAssumedIrql
)
{
    pHW_LU_EXTENSION  pLUExt = NULL;
    UCHAR             status;

    KdPrint2(("PhDskMnt::ScsiExecute: pSrb = 0x%p, CDB = 0x%X Path: %x TID: %x Lun: %x\n",
        pSrb, pSrb->Cdb[0], pSrb->PathId, pSrb->TargetId, pSrb->Lun));

#ifdef USE_SCSIPORT
    // In case of SCSIPORT (Win XP), we need to show at least one working device connected to this
    // adapter. Otherwise, SCSIPORT may not even let control requests through, which could cause
    // a scenario where even new devices cannot be added. So, for device path 0:0:0, always answer
    // 'success' (without actual response data) to some basic requests.
    if ((pSrb->PathId | pSrb->TargetId | pSrb->Lun) == 0)
    {
        ScsiExecuteRaidControllerUnit(pHBAExt, pSrb, pResult);
        goto Done;
    }
#endif

    *pResult = ResultDone;

    // Get the LU extension from port driver.
    status = ScsiGetLUExtension(pHBAExt, &pLUExt, pSrb->PathId,
        pSrb->TargetId, pSrb->Lun, LowestAssumedIrql);

    if (status != SRB_STATUS_SUCCESS)
    {
        ScsiSetError(pSrb, status);

        KdPrint(("PhDskMnt::ScsiExecute: No LUN object yet for device %d:%d:%d\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        goto Done;
    }

    // Set SCSI check conditions if LU is not yet ready
    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY
            );

        KdPrint(("PhDskMnt::ScsiExecute: Device %d:%d:%d not yet ready.\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        goto Done;
    }
    
    // Handle sufficient opcodes to support a LUN suitable for a file system. Other opcodes are failed.

    switch (pSrb->Cdb[0])
    {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SYNCHRONIZE_CACHE16:
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RESERVE_UNIT10:
    case SCSIOP_RELEASE_UNIT:
    case SCSIOP_RELEASE_UNIT10:
        ScsiSetSuccess(pSrb, 0);
        break;

    case SCSIOP_START_STOP_UNIT:
        ScsiOpStartStopUnit(pHBAExt, pLUExt, pSrb, LowestAssumedIrql);
        break;

    case SCSIOP_INQUIRY:
        ScsiOpInquiry(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_CAPACITY:
    case SCSIOP_READ_CAPACITY16:
        ScsiOpReadCapacity(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ:
    case SCSIOP_READ16:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE16:
        ScsiOpReadWrite(pHBAExt, pLUExt, pSrb, pResult, LowestAssumedIrql);
        break;

    case SCSIOP_UNMAP:
        ScsiOpUnmap(pHBAExt, pLUExt, pSrb, pResult, LowestAssumedIrql);
        break;

    case SCSIOP_READ_TOC:
        ScsiOpReadTOC(pHBAExt, pLUExt, pSrb);
        break;

#ifdef INCLUDE_EXTENDED_CD_DVD_EMULATION
    case SCSIOP_GET_CONFIGURATION:
        ScsiOpGetConfiguration(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_DISC_INFORMATION:
        ScsiOpReadDiscInformation(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_TRACK_INFORMATION:
        ScsiOpReadTrackInformation(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_GET_EVENT_STATUS:
        ScsiOpGetEventStatus(pHBAExt, pLUExt, pSrb);
        break;
#endif

    case SCSIOP_MEDIUM_REMOVAL:
        ScsiOpMediumRemoval(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_MODE_SENSE:
        ScsiOpModeSense(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_MODE_SENSE10:
        ScsiOpModeSense10(pHBAExt, pLUExt, pSrb);
        break;

    default:
        //StorPortLogError(pHBAExt, pSrb, pSrb->PathId, pSrb->TargetId, pSrb->Lun, SP_PROTOCOL_ERROR, 0x0100 | pSrb->Cdb[0]);
        KdPrint(("PhDskMnt::ScsiExecute: Unknown opcode %s (0x%X)\n", DbgGetScsiOpStr(pSrb), (int)pSrb->Cdb[0]));
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;

    } // switch (pSrb->Cdb[0])

Done:
    KdPrint2(("PhDskMnt::ScsiExecute: End: status=0x%X, *pResult=%i\n", (int)pSrb->SrbStatus, (INT)*pResult));

    return;
}                                                     // End ScsiExecute.

VOID
ScsiOpStartStopUnit(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__inout __deref PKIRQL         LowestAssumedIrql
)
{
    PCDB                     pCdb = (PCDB)pSrb->Cdb;
    NTSTATUS		     status;
    SRB_IMSCSI_REMOVE_DEVICE srb_io_data = { 0 };

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if ((pCdb->START_STOP.OperationCode != SCSIOP_START_STOP_UNIT) |
        (pCdb->START_STOP.LoadEject != 1) |
        (pCdb->START_STOP.Start != 0))
    {
        KdPrint(("PhDskMnt::ScsiOpStartStopUnit (unknown op) for device %i:%i:%i.\n",
            (int)device_extension->DeviceNumber.PathId,
            (int)device_extension->DeviceNumber.TargetId,
            (int)device_extension->DeviceNumber.Lun));

        ScsiSetSuccess(pSrb, 0);
        return;
    }

    //if (!device_extension->RemovableMedia)
    //{
    //    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) invalid for device %i:%i:%i.\n",
    //        (int)device_extension->DeviceNumber.PathId,
    //        (int)device_extension->DeviceNumber.TargetId,
    //        (int)device_extension->DeviceNumber.Lun));

    //    ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    //    return;
    //}

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) received for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    srb_io_data.DeviceNumber = device_extension->DeviceNumber;

    status = ImScsiRemoveDevice(pHBAExt, &srb_io_data.DeviceNumber, LowestAssumedIrql);

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) result for device %i:%i:%i was %#x.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        status));

    if (!NT_SUCCESS(status))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    }
    else
    {
        ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
    }
}

VOID
ScsiOpMediumRemoval(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(device_extension);

    KdPrint(("PhDskMnt::ScsiOpMediumRemoval for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if (!device_extension->RemovableMedia)
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    RtlZeroMemory(pSrb->DataBuffer, pSrb->DataTransferLength);

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

#ifdef INCLUDE_EXTENDED_CD_DVD_EMULATION

VOID
ScsiOpGetConfiguration(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PGET_CONFIGURATION_HEADER output_buffer = (PGET_CONFIGURATION_HEADER)pSrb->DataBuffer;
    // PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiOpGetConfiguration for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(GET_CONFIGURATION_HEADER)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    *(PUSHORT)output_buffer->CurrentProfile = RtlUshortByteSwap(ProfileDvdRom);

    ScsiSetSuccess(pSrb, sizeof(GET_CONFIGURATION_HEADER));
}

VOID
ScsiOpGetEventStatus(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
    __in PSCSI_REQUEST_BLOCK  pSrb
    )
{
    PUCHAR output_buffer = (PUCHAR)pSrb->DataBuffer;
    PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    cdb;

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(TRACK_INFORMATION)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    KdPrint(("PhDskMnt::ScsiOpGetEventStatus for device %i:%i:%i. Data type req = %#x\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        (int)cdb->GET_EVENT_STATUS_NOTIFICATION.Lun));

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    //output_buffer[2] = 0x0A;
    //output_buffer[7] = 0x20;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

VOID
ScsiOpReadTrackInformation(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PTRACK_INFORMATION output_buffer = (PTRACK_INFORMATION)pSrb->DataBuffer;
    PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    cdb;

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(TRACK_INFORMATION)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    KdPrint(("PhDskMnt::ScsiOpReadTrackInformation for device %i:%i:%i. Data type req = %#x\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        (int)cdb->READ_TRACK_INFORMATION.Lun));

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    //if (pSrb->DataTransferLength < 34)
    //{
    //    ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
    //    return;
    //}
    //output_buffer[2] = 0x0A;
    //output_buffer[7] = 0x20;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

VOID
ScsiOpReadDiscInformation(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PDISC_INFORMATION output_buffer = (PDISC_INFORMATION)pSrb->DataBuffer;
    PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(DISC_INFORMATION)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    KdPrint(("PhDskMnt::ScsiOpReadDiscInformation for device %i:%i:%i. Data type req = %#x\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        (int)cdb->READ_DISC_INFORMATION.Lun));

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    switch (cdb->READ_DISC_INFORMATION.Lun)
    {
    case 0x00:
        if (pSrb->DataTransferLength < sizeof(DISC_INFORMATION))
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }
        output_buffer->DiscStatus = 2;
        output_buffer->LastSessionStatus = 2;
        output_buffer->URU = 1;

        ScsiSetSuccess(pSrb, FIELD_OFFSET(DISC_INFORMATION, OPCTable));
        return;

    default:
        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_ERROR,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_INVALID_CDB,
            0);
        return;
    }
}

#endif

VOID
ScsiOpReadTOC(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PCDB       cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);
    
    KdPrint(("PhDskMnt::ScsiOpReadTOC for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if (device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE)
    {
        KdPrint(("PhDskMnt::ScsiOpReadTOC not supported for device %i:%i:%i.\n",
            (int)device_extension->DeviceNumber.PathId,
            (int)device_extension->DeviceNumber.TargetId,
            (int)device_extension->DeviceNumber.Lun));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    /*
    #define READ_TOC_FORMAT_TOC         0x00
    #define READ_TOC_FORMAT_SESSION     0x01
    #define READ_TOC_FORMAT_FULL_TOC    0x02
    #define READ_TOC_FORMAT_PMA         0x03
    #define READ_TOC_FORMAT_ATIP        0x04
    */
    
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Msf = %d\n", cdb->READ_TOC.Msf));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: LogicalUnitNumber = %d\n", cdb->READ_TOC.LogicalUnitNumber));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Format2 = %d\n", cdb->READ_TOC.Format2));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: StartingTrack = %d\n", cdb->READ_TOC.StartingTrack));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: AllocationLength = %d\n", (cdb->READ_TOC.AllocationLength[0] << 8) | cdb->READ_TOC.AllocationLength[1]));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Control = %d\n", cdb->READ_TOC.Control));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Format = %d\n", cdb->READ_TOC.Format));

    RtlZeroMemory(pSrb->DataBuffer, pSrb->DataTransferLength);

    switch (cdb->READ_TOC.Format2)
    {
    case READ_TOC_FORMAT_TOC:
    {
        PCDROM_TOC cdrom_toc = (PCDROM_TOC)pSrb->DataBuffer;
        USHORT size = FIELD_OFFSET(CDROM_TOC, TrackData) + sizeof(TRACK_DATA);

        if (pSrb->DataTransferLength < size)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            break;
        }

        *(PUSHORT)cdrom_toc->Length = RtlUshortByteSwap(size - sizeof(cdrom_toc->Length));
        cdrom_toc->FirstTrack = 1;
        cdrom_toc->LastTrack = 1;
        cdrom_toc->TrackData[0].Control = TOC_DATA_TRACK;

        ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
    }
    break;

    case READ_TOC_FORMAT_SESSION:
    {
        PCDROM_TOC_SESSION_DATA cdrom_toc = (PCDROM_TOC_SESSION_DATA)pSrb->DataBuffer;

        if (pSrb->DataTransferLength < sizeof(CDROM_TOC_SESSION_DATA))
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            break;
        }

        *(PUSHORT)cdrom_toc->Length = RtlUshortByteSwap(sizeof(CDROM_TOC_SESSION_DATA) - sizeof(cdrom_toc->Length));
        cdrom_toc->FirstCompleteSession = 1;
        cdrom_toc->LastCompleteSession = 1;
        cdrom_toc->TrackData[0].Control = TOC_DATA_TRACK; // current position data + uninterrupted data
        cdrom_toc->TrackData[0].Adr = 1;
        cdrom_toc->TrackData[0].TrackNumber = 1; // last complete track

        ScsiSetSuccess(pSrb, sizeof(CDROM_TOC_SESSION_DATA));
    }
    break;

    default:
        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_ERROR,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_INVALID_CDB,
            0);

        break;
    }
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpInquiry(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PINQUIRYDATA          pInqData = (PINQUIRYDATA)pSrb->DataBuffer;// Point to Inquiry buffer.
    PCDB                  pCdb;

    KdPrint2(("PhDskMnt::ScsiOpInquiry:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Rejected. Device not initialized.\n"));

        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY);

        goto done;
    }

    if (pSrb->DataTransferLength > 0)
    {
        RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);
        pInqData->DeviceType = pLUExt->DeviceType;
    }

    pCdb = (PCDB)pSrb->Cdb;

    if (pCdb->CDB6INQUIRY3.EnableVitalProductData == 1)
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Received VPD request for page 0x%X\n", pCdb->CDB6INQUIRY.PageCode));

        switch (pLUExt->DeviceType)
        {
        case READ_ONLY_DIRECT_ACCESS_DEVICE:
            ScsiOpVPDCdRomUnit(pHBAExt, pLUExt, pSrb);
            break;

        case DIRECT_ACCESS_DEVICE:
        case ARRAY_CONTROLLER_DEVICE:
            ScsiOpVPDDiskUnit(pHBAExt, pLUExt, pSrb);
            break;

        default:
            ScsiSetCheckCondition(
                pSrb,
                SRB_STATUS_ERROR,
                SCSI_SENSE_ILLEGAL_REQUEST,
                SCSI_ADSENSE_INVALID_CDB,
                0);
        }

        goto done;
    }
    
    if (pSrb->DataTransferLength == 0)
    {
        pSrb->DataTransferLength = pCdb->CDB6INQUIRY3.AllocationLength;
    }

    if (pSrb->DataTransferLength > 0)
    {
        RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);
        pInqData->DeviceType = pLUExt->DeviceType;
    }

    if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, VendorId))
    {
        pInqData->RemovableMedia = pLUExt->RemovableMedia;
        pInqData->CommandQueue = TRUE;

        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, VendorId) + sizeof(pInqData->VendorId))
            RtlMoveMemory(pInqData->VendorId, pHBAExt->VendorId, 8);

        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, ProductId) + sizeof(pInqData->ProductId))
            RtlMoveMemory(pInqData->ProductId, pHBAExt->ProductId, 16);
        
        if (pSrb->DataTransferLength >= FIELD_OFFSET(INQUIRYDATA, ProductRevisionLevel) + sizeof(pInqData->ProductRevisionLevel))
            RtlMoveMemory(pInqData->ProductRevisionLevel, pHBAExt->ProductRevision, 4);
    }

    ScsiSetSuccess(pSrb, min(pSrb->DataTransferLength, sizeof(INQUIRYDATA)));

done:
    KdPrint2(("PhDskMnt::ScsiOpInquiry: End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpInquiry.

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
UCHAR
ScsiGetLUExtension(
__in pHW_HBA_EXT								pHBAExt,      // Adapter device-object extension from port driver.
pHW_LU_EXTENSION * ppLUExt,
__in UCHAR									PathId,
__in UCHAR									TargetId,
__in UCHAR									Lun,
__in PKIRQL                                 LowestAssumedIrql
)
{
    PLIST_ENTRY           list_ptr;
    pHW_LU_EXTENSION *    pPortLunExt = NULL;
    UCHAR                 status;
    KLOCK_QUEUE_HANDLE    LockHandle;

    *ppLUExt = NULL;

    KdPrint2(("PhDskMnt::ScsiGetLUExtension: %d:%d:%d\n", PathId, TargetId, Lun));

    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    pPortLunExt = (pHW_LU_EXTENSION*)StoragePortGetLogicalUnit(pHBAExt, PathId, TargetId, Lun);

    if (pPortLunExt == NULL)
    {
        KdPrint(("PhDskMnt::ScsiGetLUExtension: StoragePortGetLogicalUnit failed for %d:%d:%d\n",
            PathId,
            TargetId,
            Lun));

        status = SRB_STATUS_NO_DEVICE;
        goto done;
    }

    if (*pPortLunExt != NULL)
    {
        pHW_LU_EXTENSION pLUExt = *pPortLunExt;

        if ((pLUExt->DeviceNumber.PathId != PathId) |
            (pLUExt->DeviceNumber.TargetId != TargetId) |
            (pLUExt->DeviceNumber.Lun != Lun))
        {
            DbgPrint("PhDskMnt::ScsiGetLUExtension: LUExt %p for device %i:%i:%i returned for device %i:%i:%i!\n",
                pLUExt,
                (int)pLUExt->DeviceNumber.PathId,
                (int)pLUExt->DeviceNumber.TargetId,
                (int)pLUExt->DeviceNumber.Lun,
                (int)PathId,
                (int)TargetId,
                (int)Lun);
        }
        else if (KeReadStateEvent(&pLUExt->StopThread))
        {
            DbgPrint("PhDskMnt::ScsiGetLUExtension: Device %i:%i:%i is stopping. MP reports missing to port driver.\n",
                (int)pLUExt->DeviceNumber.PathId,
                (int)pLUExt->DeviceNumber.TargetId,
                (int)pLUExt->DeviceNumber.Lun);

            *pPortLunExt = NULL;

            status = SRB_STATUS_NO_DEVICE;

            goto done;
        }
        else
        {
            if (!KeReadStateEvent(&pLUExt->Initialized))
            {
                DbgPrint("PhDskMnt::ScsiGetLUExtension: Warning: Device is not yet initialized!\n");
            }

            *ppLUExt = pLUExt;

            KdPrint2(("PhDskMnt::ScsiGetLUExtension: Device %d:%d:%d has pLUExt=0x%p\n",
                PathId, TargetId, Lun, *ppLUExt));

            status = SRB_STATUS_SUCCESS;

            goto done;
        }
    }

    for (list_ptr = pHBAExt->LUList.Flink;
        list_ptr != &pHBAExt->LUList;
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if ((object->DeviceNumber.PathId == PathId) &&
            (object->DeviceNumber.TargetId == TargetId) &&
            (object->DeviceNumber.Lun == Lun) &&
            !KeReadStateEvent(&object->StopThread))
        {
            *ppLUExt = object;
            break;
        }
    }

    if (*ppLUExt == NULL)
    {
        KdPrint2(("PhDskMnt::ScsiGetLUExtension: No saved data for Lun.\n"));

        status = SRB_STATUS_NO_DEVICE;

        goto done;
    }

    *pPortLunExt = *ppLUExt;

    status = SRB_STATUS_SUCCESS;

    KdPrint(("PhDskMnt::ScsiGetLUExtension: Device %d:%d:%d get pLUExt=0x%p\n",
        PathId, TargetId, Lun, *ppLUExt));

done:

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    KdPrint2(("PhDskMnt::ScsiGetLUExtension: End: status=0x%X\n", (int)status));

    return status;
}                                                     // End ScsiOpInquiry.

#if 0 // Report no serial number for CD/DVD units

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpVPDCdRomUnit(
    __in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     pLUExt,       // LUN device-object extension from port driver.
    __in PSCSI_REQUEST_BLOCK  pSrb
    )
{
    CDB::_CDB6INQUIRY3 * pVpdInquiry = (CDB::_CDB6INQUIRY3 *)&pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    ASSERT(pSrb->DataTransferLength>0);

    KdPrint(("PhDskMnt::ScsiOpVPDCdRomUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    switch (pVpdInquiry->PageCode)
    {
    case VPD_SUPPORTED_PAGES:
    { // Inquiry for supported pages?
        PVPD_SUPPORTED_PAGES_PAGE pSupportedPages;
        ULONG len;

        len = FIELD_OFFSET(VPD_SUPPORTED_PAGES_PAGE, SupportedPageList) + 1;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pSupportedPages = (PVPD_SUPPORTED_PAGES_PAGE)pSrb->DataBuffer;             // Point to output buffer.

        pSupportedPages->PageCode = VPD_SUPPORTED_PAGES;
        pSupportedPages->PageLength = 1;
        pSupportedPages->SupportedPageList[0] = VPD_SUPPORTED_PAGES;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    default:
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;
    }

    KdPrint2(("PhDskMnt::ScsiOpVPDCdRomUnit:  End: status=0x%X\n", (int)pSrb->SrbStatus));
}

#else

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpVPDCdRomUnit(
    __in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     pLUExt,       // LUN device-object extension from port driver.
    __in PSCSI_REQUEST_BLOCK  pSrb
    )
{
    CDB::_CDB6INQUIRY3 * pVpdInquiry = (CDB::_CDB6INQUIRY3 *)&pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    ASSERT(pSrb->DataTransferLength>0);

    KdPrint(("PhDskMnt::ScsiOpVPDCdRomUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    switch (pVpdInquiry->PageCode)
    {
    case VPD_SUPPORTED_PAGES:
    { // Inquiry for supported pages?
        PVPD_SUPPORTED_PAGES_PAGE pSupportedPages;
        ULONG len;

        len = FIELD_OFFSET(VPD_SUPPORTED_PAGES_PAGE, SupportedPageList) + 3;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pSupportedPages = (PVPD_SUPPORTED_PAGES_PAGE)pSrb->DataBuffer;             // Point to output buffer.

        pSupportedPages->PageCode = VPD_SUPPORTED_PAGES;
        pSupportedPages->PageLength = 3;
        pSupportedPages->SupportedPageList[0] = VPD_SUPPORTED_PAGES;
        pSupportedPages->SupportedPageList[1] = VPD_SERIAL_NUMBER;
        pSupportedPages->SupportedPageList[2] = VPD_DEVICE_IDENTIFIERS;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_SERIAL_NUMBER:
    {   // Inquiry for serial number?
        PVPD_SERIAL_NUMBER_PAGE pVpd;
        ULONG len;

        len = sizeof(VPD_SERIAL_NUMBER_PAGE) + 8;
        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pVpd = (PVPD_SERIAL_NUMBER_PAGE)pSrb->DataBuffer;                        // Point to output buffer.

        pVpd->PageCode = VPD_SERIAL_NUMBER;

        pVpd->PageLength = 8;
        pVpd->SerialNumber[0] = 0x31 + pLUExt->DeviceNumber.PathId;
        pVpd->SerialNumber[1] = 0x31 + pLUExt->DeviceNumber.TargetId;
        pVpd->SerialNumber[2] = 0x31 + pLUExt->DeviceNumber.Lun;
        pVpd->SerialNumber[3] = 0x34;
        pVpd->SerialNumber[4] = 0x35;
        pVpd->SerialNumber[5] = 0x36;
        pVpd->SerialNumber[6] = 0x37;
        pVpd->SerialNumber[7] = 0x38;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_DEVICE_IDENTIFIERS:
    {
        PVPD_IDENTIFICATION_PAGE IdentificationPage =
            (PVPD_IDENTIFICATION_PAGE)pSrb->DataBuffer;

        if (pSrb->DataTransferLength < sizeof(VPD_IDENTIFICATION_PAGE) +
            sizeof(VPD_IDENTIFICATION_DESCRIPTOR) + sizeof(pLUExt->UniqueId))
        {
            ScsiSetError(pSrb, SRB_STATUS_INVALID_REQUEST);
        }
        else
        {
            IdentificationPage->PageCode = VPD_DEVICE_IDENTIFIERS;
            IdentificationPage->PageLength =
                sizeof(VPD_IDENTIFICATION_DESCRIPTOR) +
                sizeof(pLUExt->UniqueId);

            PVPD_IDENTIFICATION_DESCRIPTOR pIdDescriptor =
                (PVPD_IDENTIFICATION_DESCRIPTOR)IdentificationPage->Descriptors;

            pIdDescriptor->CodeSet = VpdCodeSetBinary;
            pIdDescriptor->IdentifierType = VpdIdentifierTypeFCPHName;
            pIdDescriptor->Association = VpdAssocDevice;
            pIdDescriptor->IdentifierLength = sizeof(pLUExt->UniqueId);
            RtlCopyMemory(pIdDescriptor->Identifier, pLUExt->UniqueId,
                sizeof(pLUExt->UniqueId));

            ScsiSetSuccess(pSrb, sizeof(VPD_IDENTIFICATION_PAGE) +
                IdentificationPage->PageLength);
        }
        break;
    }

    default:
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;
    }

    KdPrint2(("PhDskMnt::ScsiOpVPDCdRomUnit:  End: status=0x%X\n", (int)pSrb->SrbStatus));
}

#endif

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpVPDDiskUnit(
    __in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     pLUExt,       // LUN device-object extension from port driver.
    __in PSCSI_REQUEST_BLOCK  pSrb
    )
{
    CDB::_CDB6INQUIRY3 * pVpdInquiry = (CDB::_CDB6INQUIRY3 *)&pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    ASSERT(pSrb->DataTransferLength>0);

    KdPrint(("PhDskMnt::ScsiOpVPDDiskUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    switch (pVpdInquiry->PageCode)
    {
    case VPD_SUPPORTED_PAGES:
    { // Inquiry for supported pages?
        PVPD_SUPPORTED_PAGES_PAGE pSupportedPages;
        ULONG len;

        len = FIELD_OFFSET(VPD_SUPPORTED_PAGES_PAGE, SupportedPageList) + 4;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pSupportedPages = (PVPD_SUPPORTED_PAGES_PAGE)pSrb->DataBuffer;             // Point to output buffer.

        pSupportedPages->PageCode = VPD_SUPPORTED_PAGES;
        pSupportedPages->PageLength = 4;
        pSupportedPages->SupportedPageList[0] = VPD_SUPPORTED_PAGES;
        pSupportedPages->SupportedPageList[1] = VPD_BLOCK_LIMITS;
        pSupportedPages->SupportedPageList[2] = VPD_LOGICAL_BLOCK_PROVISIONING;
        pSupportedPages->SupportedPageList[3] = VPD_DEVICE_IDENTIFIERS;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_BLOCK_LIMITS:
    {
        if (pSrb->DataTransferLength < 0x14)
        {
            ScsiSetError(pSrb, SRB_STATUS_INVALID_REQUEST);
        }
        else
        {
            PVPD_BLOCK_LIMITS_PAGE outputBuffer = (PVPD_BLOCK_LIMITS_PAGE)pSrb->DataBuffer;

            outputBuffer->PageCode = VPD_BLOCK_LIMITS;

            // 
            // leave outputBuffer->Descriptors[0 : 15] as '0' indicating 'not supported' for those fields. 
            // 

            if (pSrb->DataTransferLength >= 0x24)
            {
                // not worry about multiply overflow as max of DsmCapBlockCount is min(AHCI_MAX_TRANSFER_LENGTH / ATA_BLOCK_SIZE, 0xFFFF) 
                // calculate how many LBA ranges can be associated with one DSM - Trim command 
                ULONG maxLbaRangeEntryCountPerCmd;
                if (pLUExt->UseProxy &&
                    pLUExt->Proxy.connection_type == PROXY_CONNECTION::PROXY_CONNECTION_SHM)
                {
                    ULONG_PTR max_dsrs =
                        (pLUExt->Proxy.shared_memory_size - sizeof(IMDPROXY_HEADER_SIZE)) /
                        sizeof(DEVICE_DATA_SET_RANGE);

                    maxLbaRangeEntryCountPerCmd = (ULONG)min(MAXLONG, max_dsrs);
                }
                else
                {
                    maxLbaRangeEntryCountPerCmd = MAXLONG;
                }

                // calculate how many LBA can be associated with one DSM - Trim command 
                ULONG maxLbaCountPerCmd = MAXLONG;

                NT_ASSERT(maxLbaCountPerCmd > 0);

                // buffer is big enough for UNMAP information. 
                outputBuffer->PageLength[1] = 0x3C;        // must be 0x3C per spec 

                                                           // (16:19) MAXIMUM UNMAP LBA COUNT 
                REVERSE_BYTES(&outputBuffer->Descriptors[16], &maxLbaCountPerCmd);

                // (20:23) MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT 
                REVERSE_BYTES(&outputBuffer->Descriptors[20], &maxLbaRangeEntryCountPerCmd);

                // (24:27) OPTIMAL UNMAP GRANULARITY 
                // (28:31) UNMAP GRANULARITY ALIGNMENT; (28) bit7: UGAVALID 
                //leave '0' indicates un-supported. 

                // keep original 'pSrb->DataTransferLength' value. 
            }
            else
            {
                outputBuffer->PageLength[1] = 0x10;
                pSrb->DataTransferLength = 0x14;
            }

            pSrb->SrbStatus = SRB_STATUS_SUCCESS;
        }
        break;
    }

    case VPD_LOGICAL_BLOCK_PROVISIONING:
    {
        if (pSrb->DataTransferLength < 0x08)
        {
            ScsiSetError(pSrb, SRB_STATUS_INVALID_REQUEST);
        }
        else
        {
            PVPD_LOGICAL_BLOCK_PROVISIONING_PAGE outputBuffer = (PVPD_LOGICAL_BLOCK_PROVISIONING_PAGE)pSrb->DataBuffer;

            outputBuffer->PageCode = VPD_LOGICAL_BLOCK_PROVISIONING;
            outputBuffer->PageLength[1] = 0x04;      // 8 bytes data in total 
            outputBuffer->DP = 0;
            outputBuffer->ANC_SUP = pLUExt->SupportsUnmap;
            outputBuffer->LBPRZ = 0;
            outputBuffer->LBPWS10 = 0;                   // does not support WRITE SAME(10) 
            outputBuffer->LBPWS = 0;                     // does not support WRITE SAME 
            outputBuffer->LBPU = pLUExt->SupportsUnmap;  // supports UNMAP

            ScsiSetSuccess(pSrb, 0x08);
        }
        break;
    }
    
    case VPD_DEVICE_IDENTIFIERS:
    {
        PVPD_IDENTIFICATION_PAGE IdentificationPage =
            (PVPD_IDENTIFICATION_PAGE)pSrb->DataBuffer;

        ULONG required_size =
            (ULONG)(LONG_PTR)&((PVPD_IDENTIFICATION_DESCRIPTOR)
            (((VPD_IDENTIFICATION_PAGE*)0)->Descriptors))->Identifier +
            sizeof(pLUExt->UniqueId);

        if (pSrb->DataTransferLength < required_size)
        {
            ScsiSetError(pSrb, SRB_STATUS_INVALID_REQUEST);
        }
        else
        {
            IdentificationPage->PageCode = VPD_DEVICE_IDENTIFIERS;
            IdentificationPage->PageLength =
                sizeof(VPD_IDENTIFICATION_DESCRIPTOR) +
                sizeof(pLUExt->UniqueId);
            
            PVPD_IDENTIFICATION_DESCRIPTOR pIdDescriptor =
                (PVPD_IDENTIFICATION_DESCRIPTOR)IdentificationPage->Descriptors;

            pIdDescriptor->CodeSet = VpdCodeSetBinary;
            pIdDescriptor->IdentifierType = VpdIdentifierTypeFCPHName;
            pIdDescriptor->Association = VpdAssocDevice;
            pIdDescriptor->IdentifierLength = sizeof(pLUExt->UniqueId);
            RtlCopyMemory(pIdDescriptor->Identifier, pLUExt->UniqueId,
                sizeof(pLUExt->UniqueId));

            ScsiSetSuccess(pSrb, required_size);
        }
        break;
    }
    
    default:
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    }

    KdPrint2(("PhDskMnt::ScsiOpVPDDiskUnit:  End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpVPDDiskUnit().

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
#ifdef USE_SCSIPORT
VOID
ScsiOpVPDRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,          // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    CDB::_CDB6INQUIRY3 * pVpdInquiry = (CDB::_CDB6INQUIRY3 *)&pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    ASSERT(pSrb->DataTransferLength>0);

    KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    switch (pVpdInquiry->PageCode)
    {
    case VPD_SUPPORTED_PAGES:
    { // Inquiry for supported pages?
        PVPD_SUPPORTED_PAGES_PAGE pSupportedPages;
        ULONG len;

        len = sizeof(VPD_SUPPORTED_PAGES_PAGE) + 8;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pSupportedPages = (PVPD_SUPPORTED_PAGES_PAGE)pSrb->DataBuffer;             // Point to output buffer.

        pSupportedPages->PageCode = VPD_SUPPORTED_PAGES;
        pSupportedPages->PageLength = 3;
        pSupportedPages->SupportedPageList[0] = VPD_SUPPORTED_PAGES;
        pSupportedPages->SupportedPageList[1] = VPD_SERIAL_NUMBER;
        pSupportedPages->SupportedPageList[2] = VPD_DEVICE_IDENTIFIERS;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_SERIAL_NUMBER:
    {   // Inquiry for serial number?
        PVPD_SERIAL_NUMBER_PAGE pVpd;
        ULONG len;

        len = sizeof(VPD_SERIAL_NUMBER_PAGE) + 8 + 32;
        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pVpd = (PVPD_SERIAL_NUMBER_PAGE)pSrb->DataBuffer;                        // Point to output buffer.

        pVpd->PageCode = VPD_SERIAL_NUMBER;
        pVpd->PageLength = 8 + 32;

        /* Generate a changing serial number. */
        sprintf((char *)pVpd->SerialNumber, "%03d%02d%02d%03d0123456789abcdefghijABCDEFGH\n", 
            pMPDrvInfoGlobal->DrvInfoNbrMPHBAObj, 0, 0, 0);

        KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  VPD Page: %X Serial No.: %s",
            (int)pVpd->PageCode, (const char *)pVpd->SerialNumber));

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_DEVICE_IDENTIFIERS:
    { // Inquiry for device ids?
        PVPD_IDENTIFICATION_PAGE pVpid;
        PVPD_IDENTIFICATION_DESCRIPTOR pVpidDesc;
        ULONG len;

#define VPIDNameSize 32
#define VPIDName     "PSSLUNxxx"

        len = sizeof(VPD_IDENTIFICATION_PAGE) + sizeof(VPD_IDENTIFICATION_DESCRIPTOR) + VPIDNameSize;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pVpid = (PVPD_IDENTIFICATION_PAGE)pSrb->DataBuffer;                     // Point to output buffer.

        pVpid->PageCode = VPD_DEVICE_IDENTIFIERS;

        pVpidDesc =                                   // Point to first (and only) descriptor.
            (PVPD_IDENTIFICATION_DESCRIPTOR)pVpid->Descriptors;

        pVpidDesc->CodeSet = VpdCodeSetAscii;         // Identifier contains ASCII.
        pVpidDesc->IdentifierType =                   // 
            VpdIdentifierTypeFCPHName;

        /* Generate a changing serial number. */
        sprintf((char *)pVpidDesc->Identifier,
            "%03d%02d%02d%03d0123456789abcdefgh\n", pMPDrvInfoGlobal->DrvInfoNbrMPHBAObj,
            pSrb->PathId, pSrb->TargetId, pSrb->Lun);

        pVpidDesc->IdentifierLength =                 // Size of Identifier.
            (UCHAR)strlen((const char *)pVpidDesc->Identifier);

        pVpid->PageLength =                           // Show length of remainder.
            (UCHAR)(FIELD_OFFSET(VPD_IDENTIFICATION_PAGE, Descriptors) +
            FIELD_OFFSET(VPD_IDENTIFICATION_DESCRIPTOR, Identifier) +
            pVpidDesc->IdentifierLength);

        KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  VPD Page 0x83. Identifier=%.*s\n",
            pVpidDesc->IdentifierLength, pVpidDesc->Identifier));

        ScsiSetSuccess(pSrb, len);
    }
    break;
    
    default:
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    }

    KdPrint2(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpVPDRaidControllerUnit().
#endif

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpReadCapacity(
__in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PREAD_CAPACITY_DATA     readCapacity = (PREAD_CAPACITY_DATA)pSrb->DataBuffer;
    PREAD_CAPACITY_DATA_EX  readCapacity16 = (PREAD_CAPACITY_DATA_EX)pSrb->DataBuffer;
    ULARGE_INTEGER          maxBlocks;
    ULONG                   blockSize;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p, Action=0x%X\n", pHBAExt, pLUExt, pSrb, (int)pSrb->Cdb[0]));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    //if ((pLUExt == NULL) ? TRUE : ((pLUExt->DiskSize.QuadPart == 0) | (!pLUExt->Initialized)))
    //{
    //    KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Device not initialized.\n"));

    //    ScsiSetCheckCondition(
    //        pSrb,
    //        SRB_STATUS_BUSY,
    //        SCSI_SENSE_NOT_READY,
    //        SCSI_ADSENSE_LUN_NOT_READY,
    //        SCSI_SENSEQ_BECOMING_READY);

    //    return;
    //}

    blockSize = 1UL << pLUExt->BlockPower;

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity: Block Size: 0x%X\n", blockSize));

    if (pLUExt->DiskSize.QuadPart > 0)
        maxBlocks.QuadPart = (pLUExt->DiskSize.QuadPart >> pLUExt->BlockPower) - 1;
    else
        maxBlocks.QuadPart = 0;

    if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY)
        if (maxBlocks.QuadPart > MAXULONG)
            maxBlocks.QuadPart = MAXULONG;

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity: Max Blocks: 0x%I64X\n", maxBlocks));

    if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY)
    {
        REVERSE_BYTES(&readCapacity->BytesPerBlock, &blockSize);
        REVERSE_BYTES(&readCapacity->LogicalBlockAddress, &maxBlocks.LowPart);
    }
    else if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY16)
    {
        REVERSE_BYTES(&readCapacity16->BytesPerBlock, &blockSize);
        REVERSE_BYTES_QUAD(&readCapacity16->LogicalBlockAddress, &maxBlocks);
    }

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity:  End.\n"));

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
    return;
}                                                     // End ScsiOpReadCapacity.

/******************************************************************************************************/
/*                                                                                                    */
/* This routine does the setup for reading or writing. Thread ImScsiDispatchWork is going to be the   */
/* place to do the work since it gets control at PASSIVE_LEVEL and so could do real I/O, could        */
/* wait, etc, etc.                                                                                    */
/*                                                                                                    */
/******************************************************************************************************/
VOID
ScsiOpReadWrite(
__in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.        
__in PSCSI_REQUEST_BLOCK  pSrb,
__in pResultType          pResult,
__in PKIRQL               LowestAssumedIrql
)
{
    PCDB                         pCdb = (PCDB)pSrb->Cdb;
    LONGLONG                     startingSector;
    LONGLONG                     startingOffset;
    ULONG                        numBlocks;
    pMP_WorkRtnParms             pWkRtnParms;
    KLOCK_QUEUE_HANDLE           lock_handle;

    KdPrint2(("PhDskMnt::ScsiOpReadWrite:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    *pResult = ResultDone;                            // Assume no queuing.

    if ((pCdb->AsByte[0] == SCSIOP_READ16) |
        (pCdb->AsByte[0] == SCSIOP_WRITE16))
    {
        REVERSE_BYTES_QUAD(&startingSector, pCdb->CDB16.LogicalBlock);
    }
    else
    {
        startingSector = 0;
        REVERSE_BYTES(&startingSector, &pCdb->CDB10.LogicalBlockByte0);
    }

    if (startingSector & ~(MAXLONGLONG >> pLUExt->BlockPower))
    {      // Check if startingSector << blockPower fits within a LONGLONG.
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Too large sector number: %I64X\n", startingSector));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);

        return;
    }

    startingOffset = startingSector << pLUExt->BlockPower;

    numBlocks = pSrb->DataTransferLength >> pLUExt->BlockPower;

    KdPrint2(("PhDskMnt::ScsiOpReadWrite action: 0x%X, starting sector: 0x%I64X, number of blocks: 0x%X\n", (int)pSrb->Cdb[0], startingSector, numBlocks));
    KdPrint2(("PhDskMnt::ScsiOpReadWrite pSrb: 0x%p, pSrb->DataBuffer: 0x%p\n", pSrb, pSrb->DataBuffer));

    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Busy. Device not initialized.\n"));

        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY);

        return;
    }

    // Check device shutdown condition
    if (KeReadStateEvent(&pLUExt->StopThread))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Device shutting down.\n"));

        ScsiSetError(pSrb, SRB_STATUS_NO_DEVICE);

        return;
    }

    // Check write protection
    if (((pSrb->Cdb[0] == SCSIOP_WRITE) |
        (pSrb->Cdb[0] == SCSIOP_WRITE16)) &&
        pLUExt->ReadOnly)
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Write attempt on read-only device.\n"));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_DATA_PROTECT, SCSI_ADSENSE_WRITE_PROTECT, 0);

        return;
    }

    // Check disk bounds
    if ((startingSector + numBlocks) > (pLUExt->DiskSize.QuadPart >> pLUExt->BlockPower))
    {      // Starting sector beyond the bounds?
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Out of bounds: sector: %I64X, blocks: %d\n", startingSector, numBlocks));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);

        return;
    }

    // Intermediate non-paged cache
    if ((pLUExt->FileObject == NULL) &&
        (pLUExt->LastIoLength > 0) &&
        !pLUExt->SharedImage)
    {
        ImScsiAcquireLock(&pLUExt->LastIoLock, &lock_handle, *LowestAssumedIrql);

        if (((pSrb->Cdb[0] == SCSIOP_READ) |
            (pSrb->Cdb[0] == SCSIOP_READ16)) &
            (pLUExt->LastIoBuffer != NULL) &
            (pLUExt->LastIoStartSector <= startingSector) &
            ((startingOffset - (pLUExt->LastIoStartSector << pLUExt->BlockPower) + pSrb->DataTransferLength) <= pLUExt->LastIoLength))
        {
            PVOID sysaddress = NULL;
            ULONG storage_status;

            storage_status = StoragePortGetSystemAddress(pHBAExt, pSrb, &sysaddress);
            if ((storage_status != STORAGE_STATUS_SUCCESS) | (sysaddress == NULL))
            {
                ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

                DbgPrint("PhDskMnt::ScsiOpReadWrite: StorPortGetSystemAddress failed: status=0x%X address=0x%p translated=0x%p\n",
                    storage_status,
                    pSrb->DataBuffer,
                    sysaddress);

                ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
            }
            else
            {
                KdPrint(("PhDskMnt::ScsiOpReadWrite: Intermediate cache hit.\n"));

                RtlMoveMemory(
                    sysaddress,
                    (PUCHAR)pLUExt->LastIoBuffer + startingOffset - (pLUExt->LastIoStartSector << pLUExt->BlockPower),
                    pSrb->DataTransferLength
                    );

                ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

                ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
            }

            return;
        }

        ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);
    }

    pWkRtnParms = ImScsiCreateWorkItem(pHBAExt, pLUExt, pSrb);

    if (pWkRtnParms == NULL)
    {
        DbgPrint("PhDskMnt::ScsiOpReadWrite Failed to allocate work parm structure\n");

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
        return;
    }

    if (pLUExt->FileObject != NULL)
    {
        // Service work item directly in calling thread context.

        ImScsiParallelReadWriteImage(pWkRtnParms, pResult, LowestAssumedIrql);
    }
    else
    {
        // Queue work item, which will run in the System process.

        ImScsiScheduleWorkItem(pWkRtnParms, LowestAssumedIrql);

        *pResult = ResultQueued;                          // Indicate queuing.
    }

    KdPrint2(("PhDskMnt::ScsiOpReadWrite:  End. *Result=%i\n", (INT)*pResult));
}                                                     // End ScsiReadWriteSetup.

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpModeSense(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,     // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PMODE_PARAMETER_HEADER mph = (PMODE_PARAMETER_HEADER)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint2(("PhDskMnt::ScsiOpModeSense:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    if (pSrb->DataTransferLength < sizeof(MODE_PARAMETER_HEADER))
    {
        KdPrint(("PhDskMnt::ScsiOpModeSense:  Invalid request length.\n"));
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    mph->ModeDataLength = sizeof(MODE_PARAMETER_HEADER);
    if (pLUExt != NULL && pLUExt->ReadOnly)
        mph->DeviceSpecificParameter = MODE_DSP_WRITE_PROTECT;

    if (pLUExt != NULL && pLUExt->RemovableMedia)
        mph->MediumType = RemovableMedia;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpModeSense10(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,     // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PMODE_PARAMETER_HEADER10 mph = (PMODE_PARAMETER_HEADER10)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    KdPrint(("PhDskMnt::ScsiOpModeSense10:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    if (pSrb->DataTransferLength < sizeof(MODE_PARAMETER_HEADER10))
    {
        KdPrint(("PhDskMnt::ScsiOpModeSense10:  Invalid request length.\n"));
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    mph->ModeDataLength[1] = sizeof(MODE_PARAMETER_HEADER10);
    if (pLUExt != NULL && pLUExt->ReadOnly)
        mph->DeviceSpecificParameter = MODE_DSP_WRITE_PROTECT;

    if (pLUExt != NULL && pLUExt->RemovableMedia)
        mph->MediumType = RemovableMedia;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpReportLuns(
__inout         pHW_HBA_EXT         pHBAExt,   // Adapter device-object extension from port driver.
__in    __deref PSCSI_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL              LowestAssumedIrql
)
{
    UCHAR                 count;
    PLIST_ENTRY           list_ptr;
    PLUN_LIST             pLunList = (PLUN_LIST)pSrb->DataBuffer; // Point to LUN list.
    KLOCK_QUEUE_HANDLE    LockHandle;

    KdPrint(("PhDskMnt::ScsiOpReportLuns:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    // This opcode will be one of the earliest I/O requests for a new HBA (and may be received later, too).
    pHBAExt->bReportAdapterDone = TRUE;

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    for (count = 0, list_ptr = pHBAExt->LUList.Flink;
        (count < MAX_LUNS) & (list_ptr != &pHBAExt->LUList);
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if ((object->DeviceNumber.PathId == pSrb->PathId) &
            (object->DeviceNumber.TargetId == pSrb->TargetId))
            if (pSrb->DataTransferLength >= FIELD_OFFSET(LUN_LIST, Lun) + (sizeof(pLunList->Lun[0])*count))
                pLunList->Lun[count++][1] = object->DeviceNumber.Lun;
            else
                break;
    }

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    KdPrint(("PhDskMnt::ScsiOpReportLuns:  Reported %i LUNs\n", (int)count));

    pLunList->LunListLength[3] =                  // Set length needed for LUNs.
        (UCHAR)(8 * count);

    // Set the LUN numbers if there is enough room, and set only those LUNs to be reported.

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);

    KdPrint2(("PhDskMnt::ScsiOpReportLuns:  End: status=0x%X\n", (int)pSrb->SrbStatus));
}                                                     // End ScsiOpReportLuns.

VOID
ScsiOpUnmap(
    __in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.        
    __in PSCSI_REQUEST_BLOCK  pSrb,
    __in pResultType          pResult,
    __in PKIRQL               LowestAssumedIrql
)
{
    PCDB pCdb = (PCDB)pSrb->Cdb;

    PUNMAP_LIST_HEADER list = (PUNMAP_LIST_HEADER)pSrb->DataBuffer;

    USHORT length = RtlUshortByteSwap(*(PUSHORT)((PUNMAP)pCdb)->AllocationLength);

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pResult);
    UNREFERENCED_PARAMETER(LowestAssumedIrql);

    KdPrint(("PhDskMnt::ScsiOpUnmap:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Busy. Device not initialized.\n"));

        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY);

        return;
    }

    // Check device shutdown condition
    if (KeReadStateEvent(&pLUExt->StopThread))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Device shutting down.\n"));

        ScsiSetError(pSrb, SRB_STATUS_NO_DEVICE);

        return;
    }

    // Check write protection
    if (pLUExt->ReadOnly)
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Write attempt on read-only device.\n"));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_DATA_PROTECT, SCSI_ADSENSE_WRITE_PROTECT, 0);

        return;
    }

    if (!pLUExt->SupportsUnmap)
    {
        ScsiSetError(pSrb, SRB_STATUS_INVALID_REQUEST);
        return;
    }

    if (length > pSrb->DataTransferLength)
    {
        KdBreakPoint();
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    USHORT datalength = RtlUshortByteSwap(*(PUSHORT)list->DataLength);

    if ((ULONG)datalength + sizeof(list->DataLength) >
        pSrb->DataTransferLength)
    {
        KdBreakPoint();
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    USHORT descrlength = RtlUshortByteSwap(*(PUSHORT)list->BlockDescrDataLength);

    if ((ULONG)descrlength + FIELD_OFFSET(UNMAP_LIST_HEADER, Descriptors) >
        pSrb->DataTransferLength)
    {
        KdBreakPoint();
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    USHORT items = descrlength / sizeof(*list->Descriptors);

    for (USHORT i = 0; i < items; i++)
    {
        LONGLONG startingSector = RtlUlonglongByteSwap(*(PULONGLONG)list->Descriptors[i].StartingLba);
        ULONG numBlocks = RtlUlongByteSwap(*(PULONG)list->Descriptors[i].LbaCount);

        if (startingSector & ~(MAXLONGLONG >> pLUExt->BlockPower))
        {      // Check if startingSector << blockPower fits within a LONGLONG.
            KdPrint(("PhDskMnt::ScsiOpUnmap: Too large sector number: %I64X\n", startingSector));
            return;
        }

        // Check disk bounds
        if ((startingSector + numBlocks) > (pLUExt->DiskSize.QuadPart >> pLUExt->BlockPower))
        {      // Starting sector beyond the bounds?
            KdPrint(("PhDskMnt::ScsiOpUnmap: Out of bounds: sector: %I64X, blocks: %d\n", startingSector, numBlocks));
            return;
        }
    }

    pMP_WorkRtnParms pWkRtnParms = ImScsiCreateWorkItem(pHBAExt, pLUExt, pSrb);

    if (pWkRtnParms == NULL)
    {
        DbgPrint("PhDskMnt::ScsiOpUnmap Failed to allocate work parm structure\n");

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
        return;
    }

    // Queue work item, which will run in the System process.

    ImScsiScheduleWorkItem(pWkRtnParms, LowestAssumedIrql);

    *pResult = ResultQueued;                          // Indicate queuing.

    KdPrint2(("PhDskMnt::ScsiOpUnmap:  End. *Result=%i\n", (INT)*pResult));
}

VOID
ScsiOpPersistentReserveInOut(
    __in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
    __in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.        
    __in PSCSI_REQUEST_BLOCK  pSrb,
    __in pResultType          pResult,
    __in PKIRQL               LowestAssumedIrql
)
{
    pMP_WorkRtnParms pWkRtnParms = ImScsiCreateWorkItem(pHBAExt, pLUExt, pSrb);

    if (pWkRtnParms == NULL)
    {
        DbgPrint("PhDskMnt::ScsiOpPersistentReserveInOut: Failed to allocate work parm structure\n");

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
        return;
    }

    // Queue work item, which will run in the System process.

    ImScsiScheduleWorkItem(pWkRtnParms, LowestAssumedIrql);

    *pResult = ResultQueued;                          // Indicate queuing.

    KdPrint2(("PhDskMnt::ScsiOpPersistentReserveInOut:  End. *Result=%i\n", (INT)*pResult));
}

VOID
ScsiSetCheckCondition(
__in __deref PSCSI_REQUEST_BLOCK pSrb,
__in UCHAR               SrbStatus,
__in UCHAR               SenseKey,
__in UCHAR               AdditionalSenseCode,
__in UCHAR               AdditionalSenseCodeQualifier OPTIONAL
)
{
    PSENSE_DATA mph = (PSENSE_DATA)pSrb->SenseInfoBuffer;

    pSrb->SrbStatus = SrbStatus;
    pSrb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    pSrb->DataTransferLength = 0;

    RtlZeroMemory((PUCHAR)pSrb->SenseInfoBuffer, pSrb->SenseInfoBufferLength);

    if (pSrb->SenseInfoBufferLength < sizeof(SENSE_DATA))
    {
        DbgPrint("PhDskMnt::ScsiSetCheckCondition:  Insufficient sense data buffer.\n");
        return;
    }

    pSrb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;

    mph->SenseKey = SenseKey;
    mph->AdditionalSenseCode = AdditionalSenseCode;
    mph->AdditionalSenseCodeQualifier = AdditionalSenseCodeQualifier;
}

UCHAR
ScsiResetLun(
__in PVOID               pHBAExt,
__in PSCSI_REQUEST_BLOCK pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);

    ScsiSetSuccess(pSrb, 0);
    return SRB_STATUS_SUCCESS;
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnPRemoveDevice(
__in pHW_HBA_EXT             pHBAExt,// Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL            LowestAssumedIrql
)
{
    DEVICE_NUMBER remove_data = { 0 };

    KdPrint(("PhDskMnt::ScsiPnPRemoveDevice:  pHBAExt = 0x%p, pSrb = 0x%p\n", pHBAExt, pSrb));

    remove_data.LongNumber = IMSCSI_ALL_DEVICES;

    ImScsiRemoveDevice(pHBAExt, &remove_data, LowestAssumedIrql);

    pSrb->SrbStatus = SRB_STATUS_SUCCESS;

    return;
}                                                     // End ScsiPnPRemoveDevice().

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnPQueryCapabilities(
__in pHW_HBA_EXT             pHBAExt,// Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL               LowestAssumedIrql
)
{
    pHW_LU_EXTENSION          pLUExt;
    PSTOR_DEVICE_CAPABILITIES pStorageCapabilities = (PSTOR_DEVICE_CAPABILITIES)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiPnPQueryCapabilities:  pHBAExt = 0x%p, pSrb = 0x%p\n", pHBAExt, pSrb));

    // Get the LU extension from port driver.
    pSrb->SrbStatus = ScsiGetLUExtension(pHBAExt, &pLUExt, pSrb->PathId,
        pSrb->TargetId, pSrb->Lun, LowestAssumedIrql);

    if (pSrb->SrbStatus != SRB_STATUS_SUCCESS)
    {
        pSrb->DataTransferLength = 0;

        KdPrint(("PhDskMnt::ScsiPnP: No LUN object yet for device %d:%d:%d\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        return;
    }

    // Set SCSI check conditions if LU is not yet ready
    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        pSrb->DataTransferLength = 0;
        pSrb->SrbStatus = SRB_STATUS_BUSY;

        KdPrint(("PhDskMnt::ScsiPnP: Device %d:%d:%d not yet ready.\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        return;
    }

    RtlZeroMemory(pStorageCapabilities, pSrb->DataTransferLength);

    pStorageCapabilities->EjectSupported = TRUE;
    pStorageCapabilities->SilentInstall = TRUE;
    pStorageCapabilities->Removable = pLUExt->RemovableMedia;
    pStorageCapabilities->SurpriseRemovalOK = FALSE;

    pSrb->SrbStatus = SRB_STATUS_SUCCESS;

    return;
}                                                     // End ScsiPnPQueryCapabilities().

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnP(
__in pHW_HBA_EXT              pHBAExt,  // Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK  pSrb,
__inout __deref PKIRQL             LowestAssumedIrql
)
{
    KdPrint(("PhDskMnt::ScsiPnP for device %d:%d:%d:  pHBAExt = 0x%p, PnPAction = %#x, pSrb = 0x%p\n",
        pSrb->PathId, pSrb->TargetId, pSrb->Lun, pHBAExt, pSrb->PnPAction, pSrb));

    // Handle sufficient opcodes to support a LUN suitable for a file system. Other opcodes are just completed.

    switch (pSrb->PnPAction)
    {

    case StorRemoveDevice:
        ScsiPnPRemoveDevice(pHBAExt, pSrb, LowestAssumedIrql);
        break;

    case StorQueryCapabilities:
        ScsiPnPQueryCapabilities(pHBAExt, pSrb, LowestAssumedIrql);
        break;

    default:
        pSrb->SrbStatus = SRB_STATUS_SUCCESS;         // Do nothing.
    }

    KdPrint2(("PhDskMnt::ScsiPnP:  SrbStatus = 0x%X\n", pSrb->SrbStatus));

    return;
}                                                     // End ScsiPnP().

UCHAR
ScsiResetDevice(
__in PVOID               pHBAExt,
__in PSCSI_REQUEST_BLOCK pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);

    ScsiSetSuccess(pSrb, 0);
    return SRB_STATUS_SUCCESS;
}

