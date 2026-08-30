#!/usr/bin/env python3
import argparse
import os
import shutil
import struct
import subprocess
import sys

def run_cmd(cmd, check=True):
    return subprocess.run(cmd, shell=isinstance(cmd, str), check=check, capture_output=True, text=True)

def has_tool(name):
    return shutil.which(name) is not None

def format_short_name(name, ext=""):
    name = name.upper()
    ext = ext.upper()
    return f"{name:<8}{ext:<3}".encode("ascii")

class FAT32Writer:
    def __init__(self, img_path):
        self.img_path = img_path
        self.f = open(img_path, "r+b")
        self._read_bpb()

    def _read_bpb(self):
        self.f.seek(0)
        boot_sector = self.f.read(512)
        self.bytes_per_sec = struct.unpack_from("<H", boot_sector, 11)[0]
        self.sec_per_clus = struct.unpack_from("<B", boot_sector, 13)[0]
        self.reserved_sec = struct.unpack_from("<H", boot_sector, 14)[0]
        self.num_fats = struct.unpack_from("<B", boot_sector, 16)[0]
        self.fat_size_sec = struct.unpack_from("<I", boot_sector, 36)[0]
        self.root_cluster = struct.unpack_from("<I", boot_sector, 44)[0]
        self.fs_info_sec = struct.unpack_from("<H", boot_sector, 48)[0]

        self.fat_offset = self.reserved_sec * self.bytes_per_sec
        self.cluster_size = self.sec_per_clus * self.bytes_per_sec
        self.data_offset = self.fat_offset + (self.num_fats * self.fat_size_sec * self.bytes_per_sec)
        self.total_clusters = (os.path.getsize(self.img_path) - self.data_offset) // self.cluster_size

    def cluster_to_offset(self, cluster):
        return self.data_offset + (cluster - 2) * self.cluster_size

    def read_fat(self, cluster):
        offset = self.fat_offset + cluster * 4
        self.f.seek(offset)
        return struct.unpack("<I", self.f.read(4))[0] & 0x0FFFFFFF

    def write_fat(self, cluster, value):
        for i in range(self.num_fats):
            offset = self.fat_offset + (i * self.fat_size_sec * self.bytes_per_sec) + cluster * 4
            self.f.seek(offset)
            self.f.write(struct.pack("<I", value & 0x0FFFFFFF))

    def find_free_cluster(self):
        # Cluster 2 is root, start searching from 3
        for clus in range(3, self.total_clusters + 2):
            if self.read_fat(clus) == 0:
                return clus
        raise RuntimeError("Disk full: no free clusters found")

    def allocate_clusters(self, count):
        clusters = []
        for _ in range(count):
            clus = self.find_free_cluster()
            self.write_fat(clus, 0x0FFFFFFF)  # Temporarily mark as EOF
            clusters.append(clus)

        for i in range(len(clusters) - 1):
            self.write_fat(clusters[i], clusters[i + 1])
        if clusters:
            self.write_fat(clusters[-1], 0x0FFFFFFF)
        return clusters

    def read_cluster(self, cluster):
        offset = self.cluster_to_offset(cluster)
        self.f.seek(offset)
        return self.f.read(self.cluster_size)

    def write_cluster(self, cluster, data):
        offset = self.cluster_to_offset(cluster)
        self.f.seek(offset)
        if len(data) < self.cluster_size:
            data = data + b"\x00" * (self.cluster_size - len(data))
        self.f.write(data[:self.cluster_size])

    def create_dir_entry(self, short_name_8_3, attr, first_cluster, size=0):
        entry = bytearray(32)
        entry[0:11] = short_name_8_3
        entry[11] = attr
        # Cluster High (bytes 20-21) and Low (bytes 26-27)
        struct.pack_into("<H", entry, 20, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", entry, 26, first_cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, size)
        return bytes(entry)

    def add_entry_to_dir(self, dir_cluster, entry_bytes):
        curr_cluster = dir_cluster
        while True:
            data = bytearray(self.read_cluster(curr_cluster))
            for i in range(0, len(data), 32):
                if data[i] == 0x00 or data[i] == 0xE5:
                    data[i:i+32] = entry_bytes
                    self.write_cluster(curr_cluster, bytes(data))
                    return
            next_cluster = self.read_fat(curr_cluster)
            if next_cluster >= 0x0FFFFFF8:
                new_cluster = self.allocate_clusters(1)[0]
                self.write_fat(curr_cluster, new_cluster)
                new_data = bytearray(self.cluster_size)
                new_data[0:32] = entry_bytes
                self.write_cluster(new_cluster, bytes(new_data))
                return
            curr_cluster = next_cluster

    def find_entry_in_dir(self, dir_cluster, short_name_8_3):
        curr_cluster = dir_cluster
        while curr_cluster < 0x0FFFFFF8 and curr_cluster >= 2:
            data = self.read_cluster(curr_cluster)
            for i in range(0, len(data), 32):
                if data[i] == 0x00:
                    return None
                if data[i] == 0xE5:
                    continue
                if data[i:i+11] == short_name_8_3:
                    cluster_high = struct.unpack_from("<H", data, i + 20)[0]
                    cluster_low = struct.unpack_from("<H", data, i + 26)[0]
                    first_clus = (cluster_high << 16) | cluster_low
                    size = struct.unpack_from("<I", data, i + 28)[0]
                    attr = data[i + 11]
                    return {"cluster": first_clus, "size": size, "attr": attr, "entry_offset": i, "dir_cluster": curr_cluster}
            curr_cluster = self.read_fat(curr_cluster)
        return None

    def get_or_create_subdir(self, parent_cluster, dirname):
        short_name = format_short_name(dirname)
        entry = self.find_entry_in_dir(parent_cluster, short_name)
        if entry:
            return entry["cluster"]

        new_clus = self.allocate_clusters(1)[0]
        # Initialize directory cluster with . and ..
        dot_entry = self.create_dir_entry(b".          ", 0x10, new_clus)
        dotdot_entry = self.create_dir_entry(b"..         ", 0x10, parent_cluster if parent_cluster != self.root_cluster else 0)
        dir_data = bytearray(self.cluster_size)
        dir_data[0:32] = dot_entry
        dir_data[32:64] = dotdot_entry
        self.write_cluster(new_clus, bytes(dir_data))

        # Add to parent
        parent_entry = self.create_dir_entry(short_name, 0x10, new_clus)
        self.add_entry_to_dir(parent_cluster, parent_entry)
        return new_clus

    def write_file(self, dir_cluster, filename, content):
        parts = filename.split(".", 1)
        base = parts[0]
        ext = parts[1] if len(parts) > 1 else ""
        short_name = format_short_name(base, ext)

        # Free existing if any
        existing = self.find_entry_in_dir(dir_cluster, short_name)
        if existing:
            # Free clusters
            clus = existing["cluster"]
            while clus >= 2 and clus < 0x0FFFFFF8:
                next_clus = self.read_fat(clus)
                self.write_fat(clus, 0)
                clus = next_clus
            dir_data = bytearray(self.read_cluster(existing["dir_cluster"]))
            dir_data[existing["entry_offset"]] = 0xE5
            self.write_cluster(existing["dir_cluster"], bytes(dir_data))

        # Allocate clusters for new file content
        file_size = len(content)
        num_clusters = (file_size + self.cluster_size - 1) // self.cluster_size if file_size > 0 else 1
        clusters = self.allocate_clusters(num_clusters)

        for i, clus in enumerate(clusters):
            start = i * self.cluster_size
            chunk = content[start:start + self.cluster_size]
            self.write_cluster(clus, chunk)

        entry = self.create_dir_entry(short_name, 0x20, clusters[0], file_size)
        self.add_entry_to_dir(dir_cluster, entry)

    def update_fs_info(self):
        if not self.fs_info_sec:
            return
        free_count = 0
        last_allocated = 2
        for clus in range(2, self.total_clusters + 2):
            val = self.read_fat(clus)
            if val == 0:
                free_count += 1
            else:
                last_allocated = clus

        offset = self.fs_info_sec * self.bytes_per_sec
        self.f.seek(offset)
        fsinfo = bytearray(self.f.read(512))
        if len(fsinfo) == 512:
            # Check lead and struct signatures (0x41615252, 0x61417272)
            struct.pack_into("<I", fsinfo, 488, free_count)
            struct.pack_into("<I", fsinfo, 492, last_allocated)
            self.f.seek(offset)
            self.f.write(bytes(fsinfo))

    def close(self):
        self.update_fs_info()
        self.f.flush()
        self.f.close()


def copy_recursive(writer, host_dir, dest_cluster):
    for item in os.listdir(host_dir):
        host_path = os.path.join(host_dir, item)
        if os.path.isdir(host_path):
            subdir_cluster = writer.get_or_create_subdir(dest_cluster, item)
            copy_recursive(writer, host_path, subdir_cluster)
        else:
            with open(host_path, "rb") as f:
                data = f.read()
            writer.write_file(dest_cluster, item, data)

def create_or_update_image(image_path, size_bytes, bootloader_path, kernel_path, label="OZ OS", assets_dir=None):
    os.makedirs(os.path.dirname(os.path.abspath(image_path)), exist_ok=True)

    needs_format = not os.path.exists(image_path) or os.path.getsize(image_path) != size_bytes

    if needs_format:
        print(f"Creating raw disk image {image_path} ({size_bytes // (1024*1024)} MB)...")
        with open(image_path, "wb") as f:
            f.truncate(size_bytes)

        if has_tool("mkfs.fat"):
            run_cmd(["mkfs.fat", "-n", label, "-s", "2", "-f", "2", "-R", "32", "-F", "32", image_path])
        elif has_tool("mformat"):
            run_cmd(["mformat", "-i", image_path, "-F", "-v", label, "::"])
        else:
            raise RuntimeError("Neither mkfs.fat nor mformat is available to format FAT32 filesystem.")

    with open(bootloader_path, "rb") as f:
        bootloader_data = f.read()

    with open(kernel_path, "rb") as f:
        kernel_data = f.read()

    if has_tool("mcopy") and has_tool("mmd"):
        run_cmd(f"mmd -i {image_path} ::/EFI || true")
        run_cmd(f"mmd -i {image_path} ::/EFI/BOOT || true")
        run_cmd(f"mmd -i {image_path} ::/os || true")
        run_cmd(f"mcopy -o -i {image_path} {bootloader_path} ::/EFI/BOOT/BOOTX64.EFI")
        run_cmd(f"mcopy -o -i {image_path} {kernel_path} ::/os/kernel.bin")
        if assets_dir and os.path.exists(assets_dir):
            for item in os.listdir(assets_dir):
                item_path = os.path.join(assets_dir, item)
                run_cmd(f"mcopy -s -o -i {image_path} {item_path} ::/")
    else:
        writer = FAT32Writer(image_path)
        try:
            efi_cluster = writer.get_or_create_subdir(writer.root_cluster, "EFI")
            boot_cluster = writer.get_or_create_subdir(efi_cluster, "BOOT")
            writer.write_file(boot_cluster, "BOOTX64.EFI", bootloader_data)

            os_cluster = writer.get_or_create_subdir(writer.root_cluster, "os")
            writer.write_file(os_cluster, "kernel.bin", kernel_data)
            
            if assets_dir and os.path.exists(assets_dir):
                copy_recursive(writer, assets_dir, writer.root_cluster)
        finally:
            writer.close()

    print(f"Successfully populated {image_path} with BOOTX64.EFI, kernel.bin, and assets.")

def main():
    parser = argparse.ArgumentParser(description="Create and populate FAT32 OS disk image")
    parser.add_argument("--image", required=True, help="Path to output hdd.img")
    parser.add_argument("--size-mb", type=int, default=200, help="Image size in MB (default: 200)")
    parser.add_argument("--bootloader", required=True, help="Path to BOOTX64.EFI")
    parser.add_argument("--kernel", required=True, help="Path to kernel.bin")
    parser.add_argument("--assets", required=False, help="Path to an assets directory to be copied recursively to the root of the image")
    parser.add_argument("--label", default="OZ OS", help="FAT32 Volume Label")

    args = parser.parse_args()
    create_or_update_image(
        image_path=args.image,
        size_bytes=args.size_mb * 1024 * 1024,
        bootloader_path=args.bootloader,
        kernel_path=args.kernel,
        label=args.label,
        assets_dir=args.assets
    )

if __name__ == "__main__":
    main()
