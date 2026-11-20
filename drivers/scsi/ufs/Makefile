# SPDX-License-Identifier: GPL-2.0
# UFSHCD makefile
obj-$(CONFIG_SCSI_UFS_DWC_TC_PCI) += tc-dwc-g210-pci.o ufshcd-dwc.o tc-dwc-g210.o
obj-$(CONFIG_SCSI_UFS_DWC_TC_PLATFORM) += tc-dwc-g210-pltfrm.o ufshcd-dwc.o tc-dwc-g210.o
obj-$(CONFIG_SCSI_UFS_CDNS_PLATFORM) += cdns-pltfrm.o
obj-$(CONFIG_SCSI_UFS_QCOM) += ufs-qcom.o
obj-$(CONFIG_SCSI_UFSHCD) += ufshcd-core.o
ufshcd-core-y				+= ufshcd.o ufs-sysfs.o
ufshcd-core-$(CONFIG_SCSI_UFS_BSG)	+= ufs_bsg.o
# All vendor feature objects are linked INTO ufshcd-core (NothingOS layout).
# Do NOT also list them as obj-$(...) - that would link them twice.
ufshcd-core-$(CONFIG_UFSFEATURE)	+= ufsfeature.o
ufshcd-core-$(CONFIG_UFSHPB)		+= ufshpb.o
ufshcd-core-$(CONFIG_UFSTW)		+= ufstw.o
ufshcd-core-$(CONFIG_UFSHID)		+= ufshid.o
ufshcd-core-$(CONFIG_SCSI_SKHPB)	+= ufshpb_skh.o
ufshcd-core-$(CONFIG_OPLUS_FEATURE_PADL_STATISTICS) += ufs_signal_quality.o
obj-$(CONFIG_SCSI_UFSHCD_PCI) += ufshcd-pci.o
obj-$(CONFIG_SCSI_UFSHCD_PLATFORM) += ufshcd-pltfrm.o
obj-$(CONFIG_SCSI_UFS_HISI) += ufs-hisi.o
obj-$(CONFIG_SCSI_UFS_MEDIATEK) += ufs-mediatek.o
ufshcd-core-$(CONFIG_SCSI_UFS_CRYPTO) += ufshcd-crypto.o
obj-$(CONFIG_SCSI_UFS_CRYPTO_QTI) += ufshcd-crypto-qti.o