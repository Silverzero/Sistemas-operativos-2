#include "fuseLib.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/kdev_t.h>

/**
 * @brief Modifica el tamaño de los datos reservados a un inodo, reservando o liberando espacio si es necesario.
 *
 * @param idxNodoI numero de inodo a modificar
 * @param newSize tamaño que debe de tener el inodo
 * @return int
 **/
int resizeInodo(uint64_t idxNodoI, size_t newSize) {
	EstructuraNodoI *nodoI = miSistemaDeFicheros.nodosI[idxNodoI];
	char bloque[TAM_BLOQUE_BYTES];
	int i, diff = newSize - nodoI->tamArchivo;

	if(!diff)
		return 0;

	memset(bloque, 0, sizeof(char)*TAM_BLOQUE_BYTES);

	/// El fichero crece
	if(diff > 0) {

		///Borramos el contenido extra del último bloque si hay bloque y no está lleno
		if(nodoI->numBloques && nodoI->tamArchivo % TAM_BLOQUE_BYTES) {
			int bloqueActual = nodoI->idxBloques[nodoI->numBloques - 1];
			if((lseek(miSistemaDeFicheros.fdDiscoVirtual, bloqueActual * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
			        (read(miSistemaDeFicheros.fdDiscoVirtual, &bloque, TAM_BLOQUE_BYTES) == -1)) {
				perror("Falló lseek/read en resizeInodo");
				return -EIO;
			}
			int offBloque = nodoI->tamArchivo % TAM_BLOQUE_BYTES;
			int bytes2Write = (diff > (TAM_BLOQUE_BYTES - offBloque)) ? TAM_BLOQUE_BYTES - offBloque : diff;
			for(i = 0; i < bytes2Write; i++) {
				bloque[offBloque++] = 0;
			}

			if((lseek(miSistemaDeFicheros.fdDiscoVirtual, bloqueActual * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
			        (write(miSistemaDeFicheros.fdDiscoVirtual, &bloque, TAM_BLOQUE_BYTES) == -1)) {
				perror("Falló lseek/write en resizeInodo");
				return -EIO;
			}
		}

		/// Tamaño del fichero en bloques tras el aumento
		int newBloques = (newSize + TAM_BLOQUE_BYTES - 1) / TAM_BLOQUE_BYTES - nodoI->numBloques;
		if(newBloques) {
			memset(bloque, 0, sizeof(char)*TAM_BLOQUE_BYTES);

			// Comprobamos que hay suficiente espacio
			if(newBloques > miSistemaDeFicheros.superBloque.numBloquesLibres)
				return -ENOSPC;

			miSistemaDeFicheros.superBloque.numBloquesLibres -= newBloques;
			int bloqueActual = nodoI->numBloques;
			nodoI->numBloques += newBloques;

			for(i = 0; bloqueActual != nodoI->numBloques; i++) {
				if(miSistemaDeFicheros.mapaDeBits[i] == 0) {
					miSistemaDeFicheros.mapaDeBits[i] = 1;
					nodoI->idxBloques[bloqueActual] = i;
					bloqueActual++;
					//Borramos disco (necesario para truncate)
					if((lseek(miSistemaDeFicheros.fdDiscoVirtual, i * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
					        (write(miSistemaDeFicheros.fdDiscoVirtual, &bloque, TAM_BLOQUE_BYTES) == -1)) {
						perror("Falló lseek/write en resizeInodo");
						return -EIO;
					}
				}
			}
		}
		nodoI->tamArchivo += diff;

	}
	/// El fichero decrece
	else {
		//Tamaño del fichero en bloques tras el truncado
		int numBloques = (newSize + TAM_BLOQUE_BYTES - 1) / TAM_BLOQUE_BYTES;
		miSistemaDeFicheros.superBloque.numBloquesLibres += (nodoI->numBloques - numBloques);

		for(i = nodoI->numBloques; i > numBloques; i--) {
			int nBloque = nodoI->idxBloques[i - 1];
			miSistemaDeFicheros.mapaDeBits[nBloque] = 0;
			//Borramos disco (no es necesario)
			if((lseek(miSistemaDeFicheros.fdDiscoVirtual, nBloque * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
			        (write(miSistemaDeFicheros.fdDiscoVirtual, &bloque, TAM_BLOQUE_BYTES) == -1)) {
				perror("Falló lseek/write en resizeInodo");
				return -EIO;
			}
		}
		nodoI->numBloques = numBloques;
		nodoI->tamArchivo += diff;
	}
	nodoI->tiempoModificado = time(NULL);

	sync();
	
	/// Guardamos en disco el contenido de las estructuras modificadas
	escribeSuperBloque(&miSistemaDeFicheros);
	escribeMapaDeBits(&miSistemaDeFicheros);
	escribeNodoI(&miSistemaDeFicheros, idxNodoI, nodoI);

	return 0;
}

/**
 * @brief Formatea el modo de acceso de un fichero para que sea imprimible en pantalla, almacenándolo en str
 *
 * @param mode modo
 * @param str salida con el modo en forma de cadena de caracteres
 * @return void
 **/
void mode_string(mode_t mode, char *str) {
	str[0] = mode & S_IRUSR ? 'r' : '-';
	str[1] = mode & S_IWUSR ? 'w' : '-';
	str[2] = mode & S_IXUSR ? 'x' : '-';
	str[3] = mode & S_IRGRP ? 'r' : '-';
	str[4] = mode & S_IWGRP ? 'w' : '-';
	str[5] = mode & S_IXGRP ? 'x' : '-';
	str[6] = mode & S_IROTH ? 'r' : '-';
	str[7] = mode & S_IWOTH ? 'w' : '-';
	str[8] = mode & S_IXOTH ? 'x' : '-';
	str[9] = '\0';
}

/**
 * @brief Obtiene los atributos de un fichero a partir del nombre
 * 
 * Ayuda del manual de FUSE:
 * 
 * The 'st_dev' and 'st_blksize' fields are ignored. The 'st_ino' field is ignored except if the 'use_ino' mount option is given.
 *
 *		struct stat {
 *			dev_t     st_dev;     // ID of device containing file
 *			ino_t     st_ino;     // inode number
 *			mode_t    st_mode;    // protection
 *			nlink_t   st_nlink;   // number of hard links
 *			uid_t     st_uid;     // user ID of owner
 *			gid_t     st_gid;     // group ID of owner
 *			dev_t     st_rdev;    // device ID (if special file)
 *			off_t     st_size;    // total size, in bytes
 *			blksize_t st_blksize; // blocksize for file system I/O
 *			blkcnt_t  st_blocks;  // number of 512B blocks allocated
 *			time_t    st_atime;   // time of last access
 *			time_t    st_mtime;   // time of last modification (file's content were changed)
 *			time_t    st_ctime;   // time of last status change (the file's inode was last changed)
 *		};
 *
 * @param path ruta completa del fichero
 * @param stbuf atributos del fichero
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_getattr(const char *path, struct stat *stbuf) {
	EstructuraNodoI *nodoI;
	int idxDir;

	fprintf(stderr, "--->>>my_getattr: path %s\n", path);

	memset(stbuf, 0, sizeof(struct stat));

	/// Atributos del directorio raiz
	if(strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		stbuf->st_uid = getuid();
		stbuf->st_gid = getgid();
		stbuf->st_mtime = stbuf->st_ctime = miSistemaDeFicheros.superBloque.fechaCreacion;
		return 0;
	}

	/// Atributos del resto de ficheros
	if((idxDir = buscaPosDirectorio(&miSistemaDeFicheros, (char *)path + 1)) != -1) {
		nodoI = miSistemaDeFicheros.nodosI[miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI];
		stbuf->st_size = nodoI->tamArchivo;
		stbuf->st_mode = S_IFREG | 0644;
		stbuf->st_nlink = 1;
		stbuf->st_uid = getuid();
		stbuf->st_gid = getgid();
		stbuf->st_mtime = stbuf->st_ctime = nodoI->tiempoModificado;
		return 0;
	}

	return -ENOENT;
}

/**
 * @brief Lee el contenido del directorio raíz
 *
 * Ayuda del manual de FUSE:
 * 
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and passes zero to the filler function's offset.
 *    The filler function will not return '1' (unless an error happens), so the whole directory is read in a single readdir operation.
 *
 * 2) The readdir implementation keeps track of the offsets of the directory entries.
 *    It uses the offset parameter and always passes non-zero offset to the filler function. When the buffer is full
 *    (or an error happens) the filler function will return '1'.
 *
 * Function to add an entry in a readdir() operation:
 *	typedef int(* fuse_fill_dir_t)(void *buf, const char *name, const struct stat *stbuf, off_t off)
 *
 *	*Parameters
 *		-buf: the buffer passed to the readdir() operation
 *		-name: the file name of the directory entry
 *		-stat: file attributes, can be NULL
 *		-off: offset of the next entry or zero
 *	*Returns 1 if buffer is full, zero otherwise
 * 
 * @param path ruta al directorio raíz
 * @param buf buffer donde se almacenarán las entradas del directorio
 * @param filler función FUSE que se emplea para rellenar buffer
 * @param offset desplazamiento emleado por filler para rellenar buf
 * @param fi estructura FUSE asociada al directorio
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,  off_t offset, struct fuse_file_info *fi) {
	int i;

	fprintf(stderr, "--->>>my_readdir: path %s, offset %jd\n", path, (intmax_t)offset);

	if(strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	for(i = 0; i < MAX_ARCHIVOS_POR_DIRECTORIO; i++) {
		if(!(miSistemaDeFicheros.directorio.archivos[i].libre)) {
			if(filler(buf, miSistemaDeFicheros.directorio.archivos[i].nombreArchivo, NULL, 0) == 1)
				return -ENOMEM;
		}
	}

	return 0;
}

/**
 * @brief Apertura de un fichero
 *
 * Ayuda del manual de FUSE:
 * 
 * No creation (O_CREAT, O_EXCL) and by default also no truncation (O_TRUNC) flags will be passed to open().
 * If an application specifies O_TRUNC, fuse first calls truncate() and then open().
 *
 * Unless the 'default_permissions' mount option is given (limits access to the mounting user and root),
 * open should check if the operation is permitted for the given flags.
 *
 * Optionally open may also return an arbitrary filehandle in the fuse_file_info structure, which will be passed to all file operations.
 *
 *	struct fuse_file_info{
 *		int				flags				Open flags. Available in open() and release()
 *		unsigned long 	fh_old				Old file handle, don't use
 *		int 			writepage			In case of a write operation indicates if this was caused by a writepage
 *		unsigned int 	direct_io: 1		Can be filled in by open, to use direct I/O on this file
 *		unsigned int 	keep_cache: 1		Can be filled in by open, to indicate, that cached file data need not be invalidated.
 *		unsigned int 	flush: 1			Indicates a flush operation.
 *		unsigned int 	nonseekable: 1		Can be filled in by open, to indicate that the file is not seekable.
 *		unsigned int 	padding: 27			Padding. Do not use
 *		uint64_t 		fh					File handle. May be filled in by filesystem in open(). Available in all other file operations
 *		uint64_t 		lock_owner			Lock owner id.
 *		uint32_t 		poll_events			Requested poll events.
 *	}
 * 
 * @param path ruta al directorio raíz
 * @param fi estructura FUSE asociada que se asociará al fichero abierto
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_open(const char *path, struct fuse_file_info *fi) {
	int idxDir;

	fprintf(stderr, "--->>>my_open: path %s, flags %d, %"PRIu64"\n", path, fi->flags, fi->fh);

	//if(buscaInodo(path, &idxNodoI)){
	if((idxDir = buscaPosDirectorio(&miSistemaDeFicheros, (char *)path + 1)) == -1) {
		return -ENOENT;
	}

	//Guardamos el numero de inodo en file handler para utilizarlo en el resto de llamadas
	fi->fh = miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI;

	return 0;
}

/**
 * @brief Escribe datos en un fichero abierto
 *
 * Ayuda del manual de FUSE:
 *
 * Write should return exactly the number of bytes requested except on error.
 * 
 * @param path ruta al fichero abierto
 * @param buf buffer donde están los datos a escribir
 * @param size cantidad de bytes a escribir
 * @param offset desplazamiento sobre la escritura
 * @param fi estructura FUSE asociada al fichero abierto
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	char buffer[TAM_BLOQUE_BYTES];
	int bytes2Write = size, totalWrite = 0;
	EstructuraNodoI *nodoI = miSistemaDeFicheros.nodosI[fi->fh];

	fprintf(stderr, "--->>>my_write: path %s, size %zu, offset %jd, fh %"PRIu64"\n", path, size, (intmax_t)offset, fi->fh);

	//Aumentamos el tamaño del fichero si hace falta
	if(resizeInodo(fi->fh, size + offset) < 0)
		return -EIO;

	//Escribimos los datos
	while(bytes2Write) {
		int i;
		int bloqueActual, offBloque;
		bloqueActual = nodoI->idxBloques[offset / TAM_BLOQUE_BYTES];
		offBloque = offset % TAM_BLOQUE_BYTES;

		if((lseek(miSistemaDeFicheros.fdDiscoVirtual, bloqueActual * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
		        (read(miSistemaDeFicheros.fdDiscoVirtual, &buffer, TAM_BLOQUE_BYTES) == -1)) {
			perror("Falló lseek/read en my_write");
			return -EIO;
		}

		for(i = offBloque; (i < TAM_BLOQUE_BYTES) && (totalWrite < size); i++) {
			buffer[i] = buf[totalWrite++];
		}

		if((lseek(miSistemaDeFicheros.fdDiscoVirtual, bloqueActual * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
		        (write(miSistemaDeFicheros.fdDiscoVirtual, &buffer, TAM_BLOQUE_BYTES) == -1)) {
			perror("Falló lseek/write en my_write");
			return -EIO;
		}

		//Descontamos lo leido
		bytes2Write -= (i - offBloque);
		offset += i;
	}
	sync();
	
	nodoI->tiempoModificado = time(NULL);
	escribeSuperBloque(&miSistemaDeFicheros);
	escribeMapaDeBits(&miSistemaDeFicheros);
	escribeNodoI(&miSistemaDeFicheros, fi->fh, nodoI);

	return size;
}

/**
 * @brief Cerrar fichero
 *
 * Ayuda del manual de FUSE:
 * 
 * Release is called when there are no more references to an open file: all file descriptors are
 * closed and all memory mappings are unmapped.
 *
 * For every open() call there will be exactly one release() call with the same flags and file descriptor.
 * It is possible to have a file opened more than once, in which case only the last release will mean,
 * that no more reads/writes will happen on the file. The return value of release is ignored.
 * 
 * @param path ruta al fichero abierto
 * @param fi estructura FUSE asociada al fichero abierto
 * @return 0
 **/
static int my_release(const char *path, struct fuse_file_info *fi) {
	(void) path;
	(void) fi;

	fprintf(stderr, "--->>>my_release: path %s\n", path);

	return 0;
}

/**
 * @brief Crear un fichero
 *
 * Ayuda del manual de FUSE:
 * 
 * This is called for creation of all non-directory, non-symlink nodes.
 * If the filesystem defines a create() method, then for regular files that will be called instead.
 * 
 * @param path ruta al fichero que se desea crear
 * @param mode modo de creación
 * @param device dispositivo en el que se creará (contiene el major y minor number)
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_mknod(const char *path, mode_t mode, dev_t device) {
	char modebuf[10];

	mode_string(mode, modebuf);
	fprintf(stderr, "--->>>my_mknod: path %s, mode %s, major %d, minor %d\n", path, modebuf, (int)MAJOR(device), (int)MINOR(device));

	// Comprobamos que la longitud del nombre del archivo es adecuada
	if(strlen(path + 1) > miSistemaDeFicheros.superBloque.maxTamNombreArchivo) {
		return -ENAMETOOLONG;
	}

	// Comprobamos si existe un nodo-i libre
	if(miSistemaDeFicheros.numNodosLibres <= 0) {
		return -ENOSPC;
	}

	// Comprobamos que todavía cabe un archivo en el directorio (MAX_ARCHIVOS_POR_DIRECTORIO)
	if(miSistemaDeFicheros.directorio.numArchivos >= MAX_ARCHIVOS_POR_DIRECTORIO) {
		return -ENOSPC;
	}
	// Comprobamos que el fichero no exista
	if(buscaPosDirectorio(&miSistemaDeFicheros, (char *)path + 1) != -1)
		return -EEXIST;

	/// Actualizamos toda la información:
	/// mapa de bits, directorio, nodo-i, bloques de datos, superbloque ...
	int idxNodoI, idxDir;
	if((idxNodoI = buscaNodoLibre(&miSistemaDeFicheros)) == -1 || (idxDir = buscaPosDirLibre(&miSistemaDeFicheros)) == -1) {
		return -ENOSPC;
	}

	//Actualizamos el directorio raiz
	miSistemaDeFicheros.directorio.archivos[idxDir].libre = false;
	miSistemaDeFicheros.directorio.numArchivos++;
	strcpy(miSistemaDeFicheros.directorio.archivos[idxDir].nombreArchivo, path + 1);
	miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI = idxNodoI;
	miSistemaDeFicheros.numNodosLibres--;

	//Rellenamos los campos del inodo nuevo
	if(miSistemaDeFicheros.nodosI[idxNodoI] == NULL)
		miSistemaDeFicheros.nodosI[idxNodoI] = malloc(sizeof(EstructuraNodoI));

	miSistemaDeFicheros.nodosI[idxNodoI]->tamArchivo = 0;
	miSistemaDeFicheros.nodosI[idxNodoI]->numBloques = 0;
	miSistemaDeFicheros.nodosI[idxNodoI]->tiempoModificado = time(NULL);
	miSistemaDeFicheros.nodosI[idxNodoI]->libre = false;

	reservaBloquesNodosI(&miSistemaDeFicheros, miSistemaDeFicheros.nodosI[idxNodoI]->idxBloques, 0);

	escribeDirectorio(&miSistemaDeFicheros);
	escribeNodoI(&miSistemaDeFicheros, idxNodoI, miSistemaDeFicheros.nodosI[idxNodoI]);

	return 0;
}

/**
 * @brief Cambiar el tamaño de un fichero
 *
 * @param path ruta al fichero que se desea truncar
 * @param size nuevo tamaño
 * @return 0 en caso de exito y <0 en caso de error
 **/
static int my_truncate(const char *path, off_t size) {
	int idxDir;

	fprintf(stderr, "--->>>my_truncate: path %s, size %jd\n", path, size);

	if((idxDir = buscaPosDirectorio(&miSistemaDeFicheros, (char *)path + 1)) == -1) {
		return -ENOENT;
	}

	//Modificamos el tamaño del fichero
	if(resizeInodo(miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI, size) < 0)
		return -EIO;

	return 0;
}
static int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    	char buffer[TAM_BLOQUE_BYTES];
	int bytes2Read = size, totalRead = 0;
	EstructuraNodoI *nodoI = miSistemaDeFicheros.nodosI[fi->fh];

	fprintf(stderr, "--->>>my_read: path %s, size %zu, offset %jd, fh %"PRIu64"\n", path, size, (intmax_t)offset, fi->fh);


	//Leemos los datos
	while(bytes2Read) {
		int i;
		int bloqueActual, offBloque;
		bloqueActual = nodoI->idxBloques[offset / TAM_BLOQUE_BYTES];
		offBloque = offset % TAM_BLOQUE_BYTES;

		if((lseek(miSistemaDeFicheros.fdDiscoVirtual, bloqueActual * TAM_BLOQUE_BYTES, SEEK_SET) == (off_t) - 1) ||
		        (read(miSistemaDeFicheros.fdDiscoVirtual, &buffer, TAM_BLOQUE_BYTES) == -1)) {
			perror("Falló lseek/read en my_read");
			return -EIO;
		}
                
                //pasamos lo leido a buf para devolverlo
		for(i = offBloque; (i < TAM_BLOQUE_BYTES) && (totalRead < size); i++) {
			buf[totalRead++] = buffer[i];
		}

		//Descontamos lo leido
		bytes2Read -= (i - offBloque);
		offset += i;
	}
	//fflush(miSistemaDeFicheros.fdDiscoVirtual);

	return totalRead;
}

static int my_unlink(const char *path)
{
    //ponemos el tamaño del archivo a 0 para liberar sus bloques
    my_truncate(path, 0);
    //indexamos el inodo y el directorio
    int idxDir = buscaPosDirectorio(&miSistemaDeFicheros, (char *)path + 1);
    int idxNodoI = miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI;
    
    
    //Actualizamos el directorio raiz
    miSistemaDeFicheros.directorio.archivos[idxDir].libre = true;
    miSistemaDeFicheros.directorio.numArchivos--;
    strcpy(miSistemaDeFicheros.directorio.archivos[idxDir].nombreArchivo, "");
    miSistemaDeFicheros.directorio.archivos[idxDir].idxNodoI = -1;
    miSistemaDeFicheros.numNodosLibres++;

    //Rellenamos los campos del inodo nuevo
            

    miSistemaDeFicheros.nodosI[idxNodoI]->tamArchivo = 0;
    miSistemaDeFicheros.nodosI[idxNodoI]->numBloques = 0;
    miSistemaDeFicheros.nodosI[idxNodoI]->tiempoModificado = time(NULL);
    miSistemaDeFicheros.nodosI[idxNodoI]->libre = true;

    escribeDirectorio(&miSistemaDeFicheros);
    escribeNodoI(&miSistemaDeFicheros, idxNodoI, miSistemaDeFicheros.nodosI[idxNodoI]);

    return 0;
}

struct fuse_operations myFS_operations = {
	.getattr	= my_getattr,					//Obtener atributos de un fichero
	.readdir	= my_readdir,					//Leer entradas del directorio
	.truncate	= my_truncate,					//Modificar el tamaño de un fichero
	.open		= my_open,					//Abrir un fichero
	.write		= my_write,					//Escribir datos en un fichero abierto
	.release	= my_release,					//Cerrar un fichero abierto
	.mknod		= my_mknod,					//Crear un fichero nuevo
        .read           = my_read,                                      //lee datos en un fichero abierto
        .unlink         = my_unlink,                                    //borra un archivo
};

