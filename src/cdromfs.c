/*
Copyright (C) 2016 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.
*/

#include "kmalloc.h"
#include "console.h"
#include "kerneltypes.h"
#include "string.h"
#include "iso9660.h"
#include "ata.h"
#include "memory.h"
#include "cdromfs.h"

struct cdrom_volume {
	int unit;
	int root_sector;
	int root_length;
	int total_sectors;
};

struct cdrom_dirent {
	struct cdrom_volume *volume;
	int sector;
	int length;
	int isdir;
};

static struct cdrom_dirent * cdrom_dirent_create( struct cdrom_volume *volume, int sector, int length, int isdir )
{
	struct cdrom_dirent *d = kmalloc(sizeof(*d));
	if(!d) return 0;

	d->volume = volume;
	d->sector = sector;
	d->length = length;
	d->isdir = isdir;
	return d;
}

static char * cdrom_dirent_load( struct cdrom_dirent *d )
{
	int nsectors = d->length/ATAPI_BLOCKSIZE + (d->length&ATAPI_BLOCKSIZE)?1:0;
	char *data = kmalloc(nsectors*ATAPI_BLOCKSIZE);
	if(!data) return 0;

	atapi_read(d->volume->unit,data,nsectors,d->sector);
	// XXX check result

	return data;
}

static void fix_filename( char *name, int length )
{
	if(length>2) {
		length -= 2;
		name[length] = 0;
	}

	if(length>1 && name[length-1]=='.') {
		length--;
		name[length] = 0;
	}

}

/*
Read an entire cdrom file into the target address.
*/

int  cdrom_dirent_read_block( struct cdrom_dirent *d, char *buffer, int blocknum )
{
	return atapi_read( d->volume->unit, buffer, 1, d->sector + blocknum );
}

#if 0
/*
Read an entire cdrom file into the target address.
*/

int cdrom_dirent_readfile( struct cdrom_dirent *d, char *data, int length )
{
	int nsectors = d->length/ATAPI_BLOCKSIZE;
	int remainder = d->length%ATAPI_BLOCKSIZE;

	if(length<d->length) length = d->length;

	atapi_read( d->volume->unit, data, nsectors, d->sector);
	// check success here

	if(remainder>0) {
		char *page = memory_alloc_page(0);
		// XXX check success here
		atapi_read(d->volume->unit,page,1,d->sector+nsectors);
		memcpy(data + ATAPI_BLOCKSIZE*nsectors,page,remainder);
		memory_free_page(page);
	}

	return length;
}
#endif

struct cdrom_dirent * cdrom_dirent_namei( struct cdrom_dirent *d, const char *path )
{
	char *lpath = kmalloc(strlen(path)+1);
	strcpy(lpath,path);

	char *part = strtok(lpath,"/");
	while(part) {
		d = cdrom_dirent_lookup(d,part);
		if(!d) break;

		part = strtok(0,"/");
	}
	kfree(lpath);
	return d;
}

struct cdrom_dirent * cdrom_dirent_lookup( struct cdrom_dirent *dir, const char *name )
{
	char *data = cdrom_dirent_load(dir);
	if(!data) return 0;

	int data_length = dir->length;

	struct iso_9660_directory_entry *d = (struct iso_9660_directory_entry *) data;

	while(data_length>0 && d->descriptor_length>0 ) {
		fix_filename(d->ident,d->ident_length);

		if(!strcmp(name,d->ident)) {
			struct cdrom_dirent *r = cdrom_dirent_create(
				dir->volume,
				d->first_sector_little,
				d->length_little,
				d->flags & ISO_9660_EXTENT_FLAG_DIRECTORY );

			kfree(data);
			return r;
		}

		d = (struct iso_9660_directory_entry *)((char*)d+d->descriptor_length);
		data_length -= d->descriptor_length;
	}

	kfree(data);

	return 0;
}

int cdrom_dirent_read_dir( struct cdrom_dirent *dir, char *buffer, int buffer_length )
{
	char *data = cdrom_dirent_load(dir);
	if(!data) return 0;

	int data_length = dir->length;
	int total = 0;

	struct iso_9660_directory_entry *d = (struct iso_9660_directory_entry *) data;

	while(data_length>0 && d->descriptor_length>0 ) {
		fix_filename(d->ident,d->ident_length);

		if(d->ident[0]==0) {
			strcpy(buffer,".");
			buffer+=2;
			buffer_length-=2;
			total+=2;
		} else if(d->ident[0]==1) {
			strcpy(buffer,"..");
			buffer+=3;
			buffer_length-=3;
			total+=3;
		} else {
			strcpy(buffer,d->ident);
			int len = strlen(d->ident) + 1;
			buffer += len;
			buffer_length -= len;
			total += len;
		}
		d = (struct iso_9660_directory_entry *)((char*)d+d->descriptor_length);
		data_length -= d->descriptor_length;
	}

	kfree(data);

	return total;
}

int  cdrom_dirent_length( struct cdrom_dirent *d )
{
	return d->length;
}

void cdrom_dirent_close( struct cdrom_dirent *d )
{
	kfree(d);
}

struct cdrom_dirent * cdrom_volume_root( struct cdrom_volume *v )
{
	return cdrom_dirent_create(v,v->root_sector,v->root_length,1);
}

struct cdrom_volume * cdrom_volume_open( int unit )
{
	struct cdrom_volume *v = kmalloc(sizeof(*v));
	if(!v) return 0;

	struct iso_9660_volume_descriptor *d = memory_alloc_page(0);
	if(!d) {
		kfree(v);
		return 0;
	}

	printf("cdromfs: scanning atapi unit %d...\n",unit);

	int j;
	for(j=0;j<16;j++) {
		printf("cdromfs: checking volume %d\n",j);

		atapi_read(unit,d,1,j+16);
		// XXX check reuslt

		if(strncmp(d->magic,"CD001",5)) continue;

		if(d->type==ISO_9660_VOLUME_TYPE_PRIMARY) {
			v->root_sector = d->root.first_sector_little;
			v->root_length = d->root.length_little;
			v->total_sectors = d->nsectors_little;
			v->unit = unit;

			printf("cdromfs: mounted filesystem on unit %d\n",v->unit);

			memory_free_page(d);

			return v;

		} else if(d->type==ISO_9660_VOLUME_TYPE_TERMINATOR) {
			break;
		} else {
			continue;
		}
	}

	console_printf("cdromfs: no filesystem found\n");
	return 0;		
}

void cdrom_volume_close( struct cdrom_volume *v )
{
	console_printf("cdromfs: umounted filesystem from unit %d\n",v->unit);
	kfree(v);
}

