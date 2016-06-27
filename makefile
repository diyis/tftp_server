#Usar el buen compilador de GNU
CC=gcc

#Directorio para los ejecutables
EXE_DIR=./dist

#Directorio para los objetos ... aunque creo que no es necesario
OBJ_DIR=./obj


#Compilar el main y poner el resultado en dist
$(EXE_DIR)/main: main.c
	$(CC) -o $(EXE_DIR)/main main.c


#Borrar los ejecutables (Borrará todo lo que esté dentro de la carpeta $(EXE_DIR)
.PHONY: clean
clean:
	rm -vf $(EXE_DIR)/*
