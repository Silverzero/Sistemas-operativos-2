#include "myFS.h"
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


/**
 * @brief Formatea el disco virtual. Guarda el mapa de bits del super bloque y el directorio único.
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param tamDisco tamaño de disco que se pretende crear
 * @param nombreArchivo nombre del fichero de respaldo que contendrá el FS
 * @return 0 en caso de exito y <0 en caso de error
 **/
int myMkfs(MiSistemaDeFicheros *miSistemaDeFicheros, int tamDisco, char *nombreArchivo) {
	// Creamos el disco virtual:
	miSistemaDeFicheros->fdDiscoVirtual = open(nombreArchivo, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

	// Algunas comprobaciones mínimas:
	assert(sizeof(EstructuraSuperBloque) <= TAM_BLOQUE_BYTES);
	assert(sizeof(EstructuraDirectorio) <= TAM_BLOQUE_BYTES);
	int numBloques = tamDisco / TAM_BLOQUE_BYTES;
	int minNumBloques = 3 + MAX_BLOQUES_CON_NODOSI + 1;
	int maxNumBloques = NUM_BITS;
	if(numBloques < minNumBloques) {
		return -1;
	}
	if(numBloques >= maxNumBloques) {
		return -2;
	}

	/// MAPA DE BITS
	// Inicializamos el mapa de bits
	int i;
	for(i = 0; i < NUM_BITS; i++) {
		miSistemaDeFicheros->mapaDeBits[i] = 0;
	}

	// Los primeros tres bloques tendrán el superbloque, mapa de bits y directorio
	miSistemaDeFicheros->mapaDeBits[MAPA_BITS_IDX] = 1;
	miSistemaDeFicheros->mapaDeBits[SUPERBLOQUE_IDX] = 1;
	miSistemaDeFicheros->mapaDeBits[DIRECTORIO_IDX] = 1;
	// Los siguientes NUM_INODE_BLOCKS contendrán nodos-i
	for(i = 3; i < 3 + MAX_BLOQUES_CON_NODOSI; i++) {
		miSistemaDeFicheros->mapaDeBits[i] = 1;
	}
	escribeMapaDeBits(miSistemaDeFicheros);

	/// DIRECTORIO
	// Inicializamos el directorio
	miSistemaDeFicheros->directorio.numArchivos = 0;
	for(i = 0; i < MAX_ARCHIVOS_POR_DIRECTORIO; i++) {
		miSistemaDeFicheros->directorio.archivos[i].libre = 1;
	}
	escribeDirectorio(miSistemaDeFicheros);

	/// NODOS-I
	EstructuraNodoI nodoActual;
	nodoActual.libre = 1;
	for(i = 0; i < MAX_NODOSI; i++) {
		escribeNodoI(miSistemaDeFicheros, i, &nodoActual);
	}

	/// SUPERBLOQUE
	initSuperBloque(miSistemaDeFicheros, tamDisco);
	escribeSuperBloque(miSistemaDeFicheros);
	sync();

	// Al finalizar tenemos al menos un bloque
	assert(myQuota(miSistemaDeFicheros) >= 1);

	if(initNodosI(miSistemaDeFicheros)){
		myFree(miSistemaDeFicheros);
		return -3;
	}

	printf("SF: %s, %d B (%d B/bloque), %d bloques\n", nombreArchivo, tamDisco, TAM_BLOQUE_BYTES, numBloques);
	printf("1 bloque para SUPERBLOQUE (%u B)\n", (unsigned int)sizeof(EstructuraSuperBloque));
	printf("1 bloque para MAPA DE BITS, que cubre %u bloques, %u B\n", (unsigned int)NUM_BITS, (unsigned int)(NUM_BITS * TAM_BLOQUE_BYTES));
	printf("1 bloque para DIRECTORIO (%u B)\n", (unsigned int)sizeof(EstructuraDirectorio));
	printf("%d bloques para nodos-i (a %u B/nodo-i, %u nodos-i)\n", MAX_BLOQUES_CON_NODOSI, (unsigned int)sizeof(EstructuraNodoI), (unsigned int)MAX_NODOSI);
	printf("%d bloques para datos (%d B)\n", miSistemaDeFicheros->superBloque.numBloquesLibres, TAM_BLOQUE_BYTES * miSistemaDeFicheros->superBloque.numBloquesLibres);
	printf("¡Formato completado!\n");

	return 0;
}

/**
 * @brief Inicializa el superbloque 
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param tamDisco tamaño del disco de respalo del FS
 * @return void
 **/
void initSuperBloque(MiSistemaDeFicheros *miSistemaDeFicheros, int tamDisco) {
	miSistemaDeFicheros->superBloque.tamDiscoEnBloques = tamDisco / TAM_BLOQUE_BYTES;
	miSistemaDeFicheros->superBloque.numBloquesLibres = myQuota(miSistemaDeFicheros);

	miSistemaDeFicheros->superBloque.fechaCreacion = time(NULL);

	miSistemaDeFicheros->superBloque.tamBloque = TAM_BLOQUE_BYTES;
	miSistemaDeFicheros->superBloque.maxTamNombreArchivo = MAX_TAM_NOMBRE_ARCHIVO;
	miSistemaDeFicheros->superBloque.maxBloquesPorArchivo = MAX_BLOQUES_POR_ARCHIVO;
}

/**
 * @brief Libera la memoria asiciada a un FS
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return void
 **/
void myFree(MiSistemaDeFicheros *miSistemaDeFicheros) {
	int i;
	close(miSistemaDeFicheros->fdDiscoVirtual);
	for(i = 0; i < MAX_NODOSI; i++) {
		free(miSistemaDeFicheros->nodosI[i]);
		miSistemaDeFicheros->nodosI[i] = NULL;
	}
}

/**
 * @brief Escribe la información del mapa de bits en el fichero de respaldo
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return 0 en caso de exito y <0 en caso de error
 **/
int escribeMapaDeBits(MiSistemaDeFicheros *miSistemaDeFicheros) {
	if(lseek(miSistemaDeFicheros->fdDiscoVirtual, TAM_BLOQUE_BYTES * MAPA_BITS_IDX, SEEK_SET) == (off_t) - 1) {
		perror("Falló lseek en escribeMapaDeBits");
		return -1;
	}
	if(write(miSistemaDeFicheros->fdDiscoVirtual,
	         miSistemaDeFicheros->mapaDeBits, sizeof(BIT) * NUM_BITS) == -1) {
		perror("Falló write en escribeMapaDeBits");
		return -1;
	}
	sync();
	return 0;
}

/**
 * @brief Escribe la información de un inodo en el disco de respaldo.
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param numNodoI múmero de inodo
 * @param nodoI estructura del nodo a escribir (no tiene por qué coindidir con miSistemaDeFicheros.nodosI[idxNodoI])
 * @return 0 en caso de exito y <0 en caso de error
 **/
int escribeNodoI(MiSistemaDeFicheros *miSistemaDeFicheros, int numNodoI, EstructuraNodoI *nodoI) {
	int posNodoI;
	assert(numNodoI < MAX_NODOSI);
	posNodoI = calculaPosNodoI(numNodoI);

	if(lseek(miSistemaDeFicheros->fdDiscoVirtual, posNodoI, SEEK_SET) == (off_t) - 1) {
		perror("Falló lseek en escribeNodoI");
		return -1;
	}
	if(write(miSistemaDeFicheros->fdDiscoVirtual, nodoI, sizeof(EstructuraNodoI)) == -1) {
		perror("Falló write en escribeNodoI");
	}
	sync();
	return 0;
}

/**
 * @brief Escribe la información del superbloque en el disco de respaldo
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return 0 en caso de exito y <0 en caso de error
 **/
int escribeSuperBloque(MiSistemaDeFicheros *miSistemaDeFicheros) {
	if(lseek(miSistemaDeFicheros->fdDiscoVirtual, TAM_BLOQUE_BYTES * SUPERBLOQUE_IDX, SEEK_SET) == (off_t) - 1) {
		perror("Falló lseek en escribeSuperBloque");
		return -1;
	}
	if(write(miSistemaDeFicheros->fdDiscoVirtual, &(miSistemaDeFicheros->superBloque), sizeof(EstructuraSuperBloque)) == -1) {
		perror("Falló write en escribeSuperBloque");
		return -1;
	}
	sync();
	return 0;
}

/**
 * @brief Escribe la información del directorio raíz en el disco de respaldo
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return int
 **/
int escribeDirectorio(MiSistemaDeFicheros *miSistemaDeFicheros) {
	if(lseek(miSistemaDeFicheros->fdDiscoVirtual, TAM_BLOQUE_BYTES * DIRECTORIO_IDX, SEEK_SET) == (off_t) - 1) {
		perror("Falló lseek en escribeDirectorio");
		return -1;
	}
	if(write(miSistemaDeFicheros->fdDiscoVirtual, &(miSistemaDeFicheros->directorio), sizeof(EstructuraDirectorio)) == -1) {
		perror("Falló write en escribeDirectorio");
		return -1;
	}
	sync();
	return 0;
}

/**
 * @brief Calcula la posición en el disco de respaldo de un inodo
 *
 * @param numNodoI número de inodo del que se desea conocer su ubicación en disco
 * @return offset, dentro de la región de bloques de inodos del fichero de respaldo, correspondiente al inodo solicitado
 **/
int calculaPosNodoI(int numNodoI) {
	int whichInodeBlock;
	int whichInodeInBlock;
	int inodeLocation;

	whichInodeBlock = numNodoI / NODOSI_POR_BLOQUE;
	whichInodeInBlock = numNodoI % NODOSI_POR_BLOQUE;

	inodeLocation = (NODOI_IDX + whichInodeBlock) * TAM_BLOQUE_BYTES + whichInodeInBlock * sizeof(EstructuraNodoI);
	return inodeLocation;
}

/**
 * @brief Lee los inodos de disco y los almacena en nuevas estructuras miSistemaDeFicheros->nodosI[i]
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return 0 en caso de exito y <0 en caso de error
 **/
int initNodosI(MiSistemaDeFicheros *miSistemaDeFicheros) {
	int numNodoI;
	EstructuraNodoI temp;

	//Leemos los inodos uno por uno
	for(numNodoI = 0; numNodoI < MAX_NODOSI; numNodoI++) {
		if(leeNodoI(miSistemaDeFicheros, numNodoI, &temp))
			return -1;
		//Inodo libre?
		if(temp.libre) {
			miSistemaDeFicheros->nodosI[numNodoI] = NULL;
		}
		//Reservamos un inodo en memoria y copiamos el contenido de disco
		else {
			if( (miSistemaDeFicheros->nodosI[numNodoI] = malloc(sizeof(EstructuraNodoI)))==NULL){
				perror("Error al reservar nodoi");
				return -2;
			}
			copiaNodoI(miSistemaDeFicheros->nodosI[numNodoI], &temp);
			miSistemaDeFicheros->numNodosLibres--;
		}
	}
	return 0;
}

/**
 * @brief Lee un inodo del disco de respaldo
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param numNodoI número de inodo
 * @param nodoI estructura inodo que contendrá la información del disco de respaldo
 * @return 0 en caso de exito y -1 en caso de error
 **/
int leeNodoI(MiSistemaDeFicheros *miSistemaDeFicheros, int numNodoI, EstructuraNodoI *nodoI) {
	int posNodoI;
	assert(numNodoI < MAX_NODOSI);
	posNodoI = calculaPosNodoI(numNodoI);

	if( lseek(miSistemaDeFicheros->fdDiscoVirtual, posNodoI, SEEK_SET)==-1 ||
		read(miSistemaDeFicheros->fdDiscoVirtual, nodoI, sizeof(EstructuraNodoI)) != sizeof(EstructuraNodoI) ){
		perror("Error en lectura de nodoi");
		return -1;
	}
	return 0;
}

/**
 * @brief Copia el contenido de un inodo a otro
 *
 * @param dest estructura inodo destino
 * @param src estructura inodo origen
 * @return void
 **/
void copiaNodoI(EstructuraNodoI *dest, EstructuraNodoI *src) {
	int i;

	dest->numBloques = src->numBloques;
	dest->tamArchivo = src->tamArchivo;
	dest->tiempoModificado = src->tiempoModificado;
	dest->libre = src->libre;

	for(i = 0; i < MAX_BLOQUES_POR_ARCHIVO; i++)
		dest->idxBloques[i] = src->idxBloques[i];
}

/**
 * @brief Busca un inodo libre el el FS
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return número de inodo libre, -1 si no hay
 **/
int buscaNodoLibre(MiSistemaDeFicheros *miSistemaDeFicheros) {
	int i;
	for(i = 0; i < MAX_NODOSI; i++) {
		if(miSistemaDeFicheros->nodosI[i] == NULL)
			return i;
	}
	// NO hay nodos-i libres.
	return -1;
}

/**
 * @brief Busca una entrada libre en el directorio raíz
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return índice una entrada libre dentro del directorio, -1 si no hay
 **/
int buscaPosDirLibre(MiSistemaDeFicheros *miSistemaDeFicheros) {
	int i;
	for(i = 0; i < MAX_ARCHIVOS_POR_DIRECTORIO; i++) {
		if(miSistemaDeFicheros->directorio.archivos[i].libre == true)
			return i;
	}
	// NO hay entradas del directorio libres.
	return -1;
}

/**
 * @brief Busca la posición que ocupa un fichero dentro del sirectorio raíz.
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param nombre nombre del fichero a buscar
 * @return posición del directorio raíz del fichero, -1 si no está
 **/
int buscaPosDirectorio(MiSistemaDeFicheros *miSistemaDeFicheros, char *nombre) {
	int i;

	for(i = 0; i < MAX_ARCHIVOS_POR_DIRECTORIO; i++) {
		if(miSistemaDeFicheros->directorio.archivos[i].libre == false) {
			if(strcmp(nombre, miSistemaDeFicheros->directorio.archivos[i].nombreArchivo) == 0)
				return i;
		}
	}
	return -1;
}

/**
 * @brief Devuelve el número de bloques libres en el FS revisando el mapa de bits.
 *
 * @param miSistemaDeFicheros puntero al FS
 * @return número de bloques libres
 **/
int myQuota(MiSistemaDeFicheros *miSistemaDeFicheros) {
	int freeCount = 0;
	int i;
	// Calculamos el número de bloques libres
	for(i = 0; i < miSistemaDeFicheros->superBloque.tamDiscoEnBloques; i++) {
		// Ahora estamos usando uint para representar cada bit.
		// Podríamos usar todos los bits para mejorar el almacenamiento
		if(miSistemaDeFicheros->mapaDeBits[i] == 0) {
			freeCount++;
		}
	}
	return freeCount;
}

/**
 * @brief Busca bloque vacíos en el mapa de bist del sistema de ficheros reservándolos.
 *
 * @param miSistemaDeFicheros puntero al FS
 * @param idxBloques array de bloques reservados por un inodo (inicialmente vacío)
 * @param numBloques número de bloques que queremos reservar
 * @return 0 en caso de exito y -1 en caso de error
 **/
int reservaBloquesNodosI(MiSistemaDeFicheros *miSistemaDeFicheros, DISK_LBA idxBloques[], int numBloques) {
	int i = 0;
	int bloqueActual = 0;

	while((bloqueActual < numBloques) && (i < NUM_BITS)) {
		if(miSistemaDeFicheros->mapaDeBits[i] == 0) {
			miSistemaDeFicheros->mapaDeBits[i] = 1;
			idxBloques[bloqueActual] = i;
			bloqueActual++;
		}
		++i;
	}
	
	return (i==NUM_BITS)?-1:0;
}
