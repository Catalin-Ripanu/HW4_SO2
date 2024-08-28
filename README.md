# HW4_SO2

A software RAID module that uses a logical block device that will read and write data from two physical devices, ensuring the consistency and synchronization of data from the two physical devices.

The type of RAID implemented will be similar to a RAID 1.

This kernel module implements the RAID software functionality. Software RAID [1] provides an abstraction between the logical device and the physical devices. The implementation will use RAID scheme 1 [2].

The used virtual machine (qemu) has two hard disks that represents the physical devices: */dev/vdb* and */dev/vdc*. The operating system provides a logical device (block type) that will interface the access from the user space. Writing requests to the logical device will result in two writes, one for each hard disk. Hard disks are not partitioned. It is considered that each hard disk has a single partition that covers the entire disk.

Each partition will store a sector along with an associated checksum (CRC32) to ensure error recovery. At each reading, the related information from both partitions is read. If a sector of the first partition has corrupt data (CRC value is wrong) then the sector on the second partition will be read; at the same time the sector of the first partition will be corrected. Similar in the case of a reading of a corrupt sector on the second partition. If a sector has incorrect CRC values on both partitions, an appropriate error code will be returned.

To ensure error recovery, a CRC code is associated with each sector. CRC codes are stored by LOGICAL_DISK_SIZE byte of the partition (macro defined in the assignment header). The disk structure will have the following layout:

```
+-----------+-----------+-----------+     +---+---+---+
|  sector1  |  sector2  |  sector3  |.....|C1 |C2 |C3 |
+-----------+-----------+-----------+     +---+---+---+
```

where C1, C2, C3 are the values CRC sectors sector1, sector2, sector3. The CRC area is found immediately after the LOGICAL_DISK_SIZE bytes of the partition.

As a seed for CRC 0(zero) was used.

## Implementation details:

- The logical device will be accessed as a block device with the major SSR_MAJOR and minor SSR_FIRST_MINOR under the name /dev/ssr (via the macro LOGICAL_DISK_NAME)

- The virtual device (LOGICAL_DISK_NAME - /dev/ssr) will have the capacity of LOGICAL_DISK_SECTORS (use set_capacity with the struct gendisk structure)

- The two disks are represented by the devices /dev/vdb, respectively /dev/vdc, defined by means of macros PHYSICAL_DISK1_NAME, respectively PHYSICAL_DISK2_NAME

- To work with the struct block _device structure associated with a physical device, the blkdev_get_by_path and blkdev_put functions were used

- When generating a struct bio structure, its size must be multiple of the disk sector size (KERNEL_SECTOR_SIZE)

- Used bio_endio() to signal the completion of processing a bio structure

- Useful macro definitions can be found in the assignment support header

[1]: https://en.wikipedia.org/wiki/RAID#Software-based_RAID
[2]: https://en.wikipedia.org/wiki/RAID#Standard_levels
