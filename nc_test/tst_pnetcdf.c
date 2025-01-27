/* This is part of the netCDF package.
   Copyright 2018 University Corporation for Atmospheric Research/Unidata
   See COPYRIGHT file for conditions of use.

   Test fix of bug involving creation of a file with PnetCDF APIs,
   then opening and modifying the file with netcdf.

   Author: Wei-keng Liao.
*/

#include <nc_tests.h>
#include "err_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <netcdf.h>
#include <netcdf_par.h>
#include <assert.h>

#define NVARS 6
#define NX    5
#define FILENAME "tst_pnetcdf.nc"

#define CHK_ERR(e) { \
    if (e != NC_NOERR) { \
        printf("Error at %s:%d : %s\n", __FILE__,__LINE__, nc_strerror(e)); \
        goto fn_exit; \
    } \
}

#define EXP_ERR(e, exp) { \
    if (e != exp) { \
        printf("Error at %s:%d : expect "#exp" but got %d\n", __FILE__,__LINE__, e); \
    } \
}


int main(int argc, char* argv[])
{
    int i, j, rank, nprocs, ncid, cmode, varid[NVARS], dimid[2], *buf;
    int st, nerrs=0;
    char str[32];
    size_t start[2], count[2];
    MPI_Comm comm=MPI_COMM_SELF;
    MPI_Info info=MPI_INFO_NULL;
    char file_name[NC_MAX_NAME + 1];

    printf("\n*** Testing bug fix with changing PnetCDF variable offsets...");

    MPI_Init(&argc,&argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (nprocs > 1 && rank == 0)
        printf("This test program is intended to run on ONE process\n");
    if (rank > 0) goto fn_exit;

    /* first, use PnetCDF to create a file with default header/variable alignment */
#ifdef DISABLE_PNETCDF_ALIGNMENT
    MPI_Info_create(&info);
    MPI_Info_set(info, "nc_header_align_size", "1");
    MPI_Info_set(info, "nc_var_align_size",    "1");
#endif

    cmode = NC_CLOBBER;
    sprintf(file_name, "%s/%s", TEMP_LARGE, FILENAME);
    st = nc_create_par(file_name, cmode, comm, info, &ncid);
#ifdef USE_PNETCDF
    CHK_ERR(st)
#else
    EXP_ERR(st, NC_ENOTBUILT)
    goto fn_exit;
#endif

    /* define dimension */
    st = nc_def_dim(ncid, "Y", NC_UNLIMITED, &dimid[0]); CHK_ERR(st)
    st = nc_def_dim(ncid, "X", NX,           &dimid[1]); CHK_ERR(st)

    /* Odd numbers are fixed variables, even numbers are record variables */
    for (i=0; i<NVARS; i++) {
        if (i%2) {
            sprintf(str,"fixed_var_%d",i);
            st = nc_def_var(ncid, str, NC_INT, 1, dimid+1, &varid[i]); CHK_ERR(st)
        }
        else {
            sprintf(str,"record_var_%d",i);
            st = nc_def_var(ncid, str, NC_INT, 2, dimid, &varid[i]); CHK_ERR(st)
        }
    }
    st = nc_enddef(ncid); CHK_ERR(st)

    /* Note NC_INDEPENDENT is the default */
    st = nc_var_par_access(ncid, NC_GLOBAL, NC_INDEPENDENT); CHK_ERR(st)

    /* write all variables */
    buf = (int*) malloc(NX * sizeof(int));
    for (i=0; i<NVARS; i++) {
        for (j=0; j<NX; j++) buf[j] = i*10 + j;
        if (i%2) {
            start[0] = 0; count[0] = NX;
            st = nc_put_vara_int(ncid, varid[i], start, count, buf); CHK_ERR(st)
        }
        else {
            start[0] = 0; start[1] = 0;
            count[0] = 1; count[1] = NX;
            st = nc_put_vara_int(ncid, varid[i], start, count, buf); CHK_ERR(st)
        }
    }
    st = nc_close(ncid); CHK_ERR(st)
    if (info != MPI_INFO_NULL) MPI_Info_free(&info);

    /* re-open the file with netCDF (parallel) and enter define mode */
    st = nc_open_par(file_name, NC_WRITE, comm, info, &ncid); CHK_ERR(st)

    st = nc_redef(ncid); CHK_ERR(st)

    /* add attributes to make header grow */
    for (i=0; i<NVARS; i++) {
        sprintf(str, "annotation_for_var_%d",i);
        st = nc_put_att_text(ncid, varid[i], "text_attr", strlen(str), str); CHK_ERR(st)
    }
    st = nc_enddef(ncid); CHK_ERR(st)

    /* read variables and check their contents */
    for (i=0; i<NVARS; i++) {
        for (j=0; j<NX; j++) buf[j] = -1;
        if (i%2) {
            start[0] = 0; count[0] = NX;
            st = nc_get_var_int(ncid, varid[i], buf); CHK_ERR(st)
            for (j=0; j<NX; j++)
                if (buf[j] != i*10 + j)
                    printf("unexpected read value var i=%d buf[j=%d]=%d should be %d\n",i,j,buf[j],i*10+j);
        }
        else {
            start[0] = 0; start[1] = 0;
            count[0] = 1; count[1] = NX;
            st = nc_get_vara_int(ncid, varid[i], start, count, buf); CHK_ERR(st)
            for (j=0; j<NX; j++)
                if (buf[j] != i*10+j)
                    printf("unexpected read value var i=%d buf[j=%d]=%d should be %d\n",i,j,buf[j],i*10+j);
        }
    }
    st = nc_close(ncid); CHK_ERR(st)
    free(buf);

fn_exit:
    MPI_Finalize();
    err = nerrs;
    SUMMARIZE_ERR;
    FINAL_RESULTS;
    return (nerrs > 0);
}

/*
    Compile:
        mpicc -g -o nc_pnc nc_pnc.c -lnetcdf -lcurl -lhdf5_hl -lhdf5 -lpnetcdf -lz -lm

    Run:
        nc_pnc

    Standard Output:
        At the time of this test is written, I used the following libraries.
            HDF5    version 1.8.10
            netCDF  version 4.2.1.1 and
            PnetCDF version 1.3.1

        If macro DISABLE_PNETCDF_ALIGNMENT is defined (i.e. disable PnetCDF
        alignment) then there is no standard output.

        If macro DISABLE_PNETCDF_ALIGNMENT is NOT defined (i.e. default PnetCDF
        alignment) then this test reports unexpected read values below.

         unexpected read value var i=1 buf[j=0]=0 should be 10
         unexpected read value var i=1 buf[j=1]=0 should be 11
         unexpected read value var i=1 buf[j=2]=0 should be 12
         unexpected read value var i=1 buf[j=3]=0 should be 13
         unexpected read value var i=1 buf[j=4]=0 should be 14
         unexpected read value var i=3 buf[j=0]=0 should be 30
         unexpected read value var i=3 buf[j=1]=0 should be 31
         unexpected read value var i=3 buf[j=2]=0 should be 32
         unexpected read value var i=3 buf[j=3]=0 should be 33
         unexpected read value var i=3 buf[j=4]=0 should be 34
         unexpected read value var i=5 buf[j=0]=0 should be 50
         unexpected read value var i=5 buf[j=1]=0 should be 51
         unexpected read value var i=5 buf[j=2]=0 should be 52
         unexpected read value var i=5 buf[j=3]=0 should be 53
         unexpected read value var i=5 buf[j=4]=0 should be 54
*/
