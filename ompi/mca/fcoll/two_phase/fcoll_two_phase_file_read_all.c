/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2011 University of Houston. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"
#include "fcoll_two_phase.h"

#include "mpi.h"
#include "ompi/constants.h"
#include "ompi/mca/fcoll/fcoll.h"
#include "ompi/mca/io/ompio/io_ompio.h"
#include "ompi/mca/io/io.h"
#include "math.h"
#include "ompi/mca/pml/pml.h"
#include <unistd.h>

int
mca_fcoll_two_phase_file_read_all (mca_io_ompio_file_t *fh, 
                                   void *buf, 
                                   int count, 
                                   struct ompi_datatype_t *datatype, 
                                   ompi_status_public_t *status)
{
    size_t total_bytes_read = 0;  /* total bytes that have been read*/
    size_t total_bytes = 0;          /* total bytes to be read */
    size_t total_bytes_global = 0;
    size_t bytes_to_read_in_cycle = 0; /* left to be read in a cycle*/
    size_t max_data = 0; 
    size_t bytes_remaining = 0;
    size_t *bytes_rem = NULL;
    size_t *prev_bytes_rem = NULL;
    size_t stripe_size =0;

    int index = 0;
    int current_index = 0;
    int *current = NULL;
    int *previous = NULL;
    int cycles = 0;
    int i=0, j=0, x=0, n=0;
    int blocks = 0;
    int bytes_left = 0;
    int two_phase_num_io_procs = 0;

    /* array that contains the sorted indices of the global_iov */
    int *sorted = NULL;
    int *displs = NULL;
    int *bytes_per_process = NULL;
    int *bytes_received = NULL;

    /* iovec structure and count of the buffer passed in */
    uint32_t iov_count = 0;
    struct iovec *decoded_iov = NULL;

    int global_fview_count = 0;
    struct iovec *global_fview = NULL;

    int local_count = 0;
    struct iovec *iov = NULL;

    int broken_count = 0;
    struct iovec *broken_iovec = NULL;

    int *fview_count = NULL;

    int global_count = 0;
    char *global_buf = NULL;

    if (opal_datatype_is_contiguous_memory_layout(&datatype->super,1)) {
        fh->f_flags |= OMPIO_CONTIGUOUS_MEMORY;
    }

    /**************************************************************************
     ** In case the data is not contigous in memory, decode it into an iovec **
     **************************************************************************/
    if (!(fh->f_flags & OMPIO_CONTIGUOUS_MEMORY)) {
        ompi_io_ompio_decode_datatype (fh,
                                       datatype,
                                       count,
                                       buf,
                                       &max_data,
                                       &decoded_iov,
                                       &iov_count);
    }
    else {
        max_data = count * datatype->super.size;
    }

    if (! (fh->f_flags & OMPIO_AGGREGATOR_IS_SET)) {
        if (-1 == mca_fcoll_two_phase_num_io_procs) {
            ompi_io_ompio_set_aggregator_props (fh, 
                                                mca_fcoll_two_phase_num_io_procs,
                                                max_data);
            two_phase_num_io_procs = 
                ceil((float)fh->f_size/fh->f_procs_per_group);
        }
        fh->f_aggregator_index = 
            ceil((float)fh->f_size/two_phase_num_io_procs);
        if (fh->f_aggregator_index * two_phase_num_io_procs > fh->f_size) {
            two_phase_num_io_procs = 
                ceil((float)fh->f_size/fh->f_aggregator_index);
        }
    }

    /*********************************************************************
     *** Generate the File offsets/lengths corresponding to this read ***
     ********************************************************************/
    ompi_io_ompio_generate_current_file_view (fh, 
                                              max_data, 
                                              &iov, 
                                              &local_count);
    /*    
    for (i=0 ; i<local_count ; i++) {
        printf("%d: OFFSET: %d   LENGTH: %d\n",
               fh->f_rank,
               iov[i].iov_base,
               iov[i].iov_len);
    }
    */

    /*************************************************************
     * Breakdown the file view at each process per OST then send *
     * each portion of the file view t0 the corresp aggregator   *
     *************************************************************/

    fh->f_comm->c_coll.coll_allreduce (&max_data,
                                       &total_bytes,
                                       1,
                                       MPI_DOUBLE,
                                       MPI_SUM,
                                       fh->f_comm,
                                       fh->f_comm->c_coll.coll_allreduce_module);

    stripe_size = ceil((float)total_bytes/two_phase_num_io_procs);

    ompi_io_ompio_break_file_view (fh,
                                   iov,
                                   local_count,
                                   two_phase_num_io_procs,
                                   stripe_size,
                                   &broken_iovec,
                                   &broken_count);
    /*
    for (i=0 ; i<broken_count ; i++) {
        printf("%d: OFFSET: %d   LENGTH: %d\n",
               fh->f_rank,
               broken_iovec[i].iov_base,
               broken_iovec[i].iov_len);
    }
    */

    if (NULL != iov) {
        free (iov);
        iov = NULL;
    }

    ompi_io_ompio_distribute_file_view (fh,
                                        broken_iovec,
                                        broken_count,
                                        two_phase_num_io_procs,
                                        stripe_size,
                                        &fview_count,
                                        &global_fview,
                                        &global_fview_count);
    /*
    for (i=0 ; i<global_fview_count ; i++) {
        printf("%d: OFFSET: %d   LENGTH: %d\n",
               fh->f_rank,
               global_fview[i].iov_base,
               global_fview[i].iov_len);
    }
    */

    total_bytes = 0;
    if (0 == fh->f_rank%fh->f_aggregator_index) {
        if (global_fview_count) {
            for (i=0 ; i<global_fview_count ; i++) {
                total_bytes += global_fview[i].iov_len;
            }
            /* sort it */
            sorted = (int *)malloc (global_fview_count * sizeof(int));
            if (NULL == sorted) {
                opal_output (1, "OUT OF MEMORY\n");
                return OMPI_ERR_OUT_OF_RESOURCE;
            }
            ompi_io_ompio_sort_iovec (global_fview, global_fview_count, sorted);
        }
        bytes_per_process = (int *) malloc (fh->f_size * sizeof (int));
        if (NULL == bytes_per_process) {
            opal_output (1, "OUT OF MEMORY\n");
            return OMPI_ERR_OUT_OF_RESOURCE;
        }
        displs = (int *) malloc (fh->f_size * sizeof (int));
        if (NULL == displs) {
            opal_output (1, "OUT OF MEMORY\n");
            return OMPI_ERR_OUT_OF_RESOURCE;
        }
    }
    fh->f_comm->c_coll.coll_allreduce (&total_bytes,
                                       &total_bytes_global,
                                       1,
                                       MPI_DOUBLE,
                                       MPI_MAX,
                                       fh->f_comm,
                                       fh->f_comm->c_coll.coll_allreduce_module);
    
    bytes_received = (int *)malloc (two_phase_num_io_procs * sizeof (int));
    if (NULL == bytes_received) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    current = (int *)malloc (two_phase_num_io_procs * sizeof (int));
    if (NULL == current) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    previous = (int *)malloc (two_phase_num_io_procs * sizeof (int));
    if (NULL == previous) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    bytes_rem = (size_t *)malloc (two_phase_num_io_procs * sizeof (size_t));
    if (NULL == bytes_rem) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    prev_bytes_rem = (size_t *)malloc (two_phase_num_io_procs * sizeof (size_t));
    if (NULL == prev_bytes_rem) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    memset(current, 0x0, two_phase_num_io_procs*sizeof(int));
    memset(previous, 0x0, two_phase_num_io_procs*sizeof(int));
    memset(bytes_rem, 0x0, two_phase_num_io_procs*sizeof(size_t));
    memset(prev_bytes_rem, 0x0, two_phase_num_io_procs*sizeof(size_t));

    cycles = ceil ((float)total_bytes_global/
                   mca_fcoll_two_phase_cycle_buffer_size);

    for (index = 0; index < cycles; index++) {
        int k = 0;
        size_t total_bytes_recv = 0;
        size_t temp = 0;
        global_count = 0;

        memset(bytes_received, 0x0, two_phase_num_io_procs*sizeof(int));
        if (0 == fh->f_rank%fh->f_aggregator_index) {
            memset(displs, 0x0, fh->f_size*sizeof(int));
            memset(bytes_per_process, 0x0, fh->f_size*sizeof(int));

            if ((int)total_bytes > mca_fcoll_two_phase_cycle_buffer_size) {
                bytes_to_read_in_cycle = mca_fcoll_two_phase_cycle_buffer_size;
            }
            else {
                bytes_to_read_in_cycle = total_bytes;
            }
        }
        /*
        printf ("****%d: Total_bytes: %d  CYCLE %d   Bytes %d OFFSET %d******\n",
                fh->f_rank,
                total_bytes,
                index,
                bytes_to_read_in_cycle,
                fh->f_offset);
        sleep(1);
        */
        /**********************************************************
         **Gather the Data from all the processes at the readers **
         *********************************************************/

        /* Calculate how much data will be contributed in this cycle 
           by each process*/
        for (k=0 ; k<two_phase_num_io_procs ; k++) {
            previous[k] = current[k];
            prev_bytes_rem[k] = bytes_rem[k];
            temp = (k * stripe_size) + 
                ((size_t)mca_fcoll_two_phase_cycle_buffer_size * (index+1)) + fh->f_offset;

            while (current[k] < broken_count) {
                if (k*stripe_size+fh->f_offset >
                    (size_t)broken_iovec[current[k]].iov_base ||
                    (k+1)*stripe_size+fh->f_offset <=
                    (size_t)broken_iovec[current[k]].iov_base) {
                    if ((k+1)*stripe_size+fh->f_offset <= 
                        (size_t)broken_iovec[current[k]].iov_base) {
                        break;
                    }
                    current[k] ++;
                    previous[k] = current[k];
                    continue;
                }
                if (temp >= 
                    (size_t)((OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base +
                             broken_iovec[current[k]].iov_len)) {
                    if (bytes_rem[k]) {
                        bytes_received[k] += bytes_rem[k];
                        total_bytes_recv += bytes_rem[k];
                        bytes_rem[k] = 0;
                    }
                    else {
                        bytes_received[k] += broken_iovec[current[k]].iov_len;
                        total_bytes_recv += broken_iovec[current[k]].iov_len;
                    }
                    current[k] ++;
                }
                else {
                    if (bytes_rem[k]) {
                        bytes_received[k] += temp - 
                            ((broken_iovec[current[k]].iov_len - bytes_rem[k]) + 
                             (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base);
                        total_bytes_recv += temp - 
                            ((broken_iovec[current[k]].iov_len - bytes_rem[k]) + 
                             (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base);
                        bytes_rem[k] -= temp - 
                            ((broken_iovec[current[k]].iov_len - bytes_rem[k]) + 
                             (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base);
                        break;
                    }
                    else {
                        if (temp > (size_t)broken_iovec[current[k]].iov_base) {
                            bytes_received[k] += temp - 
                                (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base;
                            total_bytes_recv += temp - 
                                (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base;
                            bytes_rem[k] = broken_iovec[current[k]].iov_len - 
                                (temp - 
                                 (OPAL_PTRDIFF_TYPE)broken_iovec[current[k]].iov_base);
                            break;
                        }
                        else {
                            break;
                        }
                    }
                }
            }
        }
        /*
        for (i=0 ; i<two_phase_num_io_procs ; i++) {
            printf ("%d sending %d to %d\n",fh->f_rank, 
                    bytes_received[i], i);
        }
        sleep(1);
        */
        if (0 == fh->f_rank%fh->f_aggregator_index && bytes_to_read_in_cycle) {
            /* Calculate how much data will be recieved this cycle 
               by each aggregator*/
            while (bytes_to_read_in_cycle) {
                blocks = fview_count[0];
                for (j=0 ; j<fh->f_size ; j++) {
                    if (sorted[current_index] < blocks) {
                        n = j;
                        break;
                    }
                    else {
                        blocks += fview_count[j+1];
                    }
                }
                if (bytes_remaining) {
                    if (bytes_remaining <= bytes_to_read_in_cycle) {
                        bytes_per_process[n] += bytes_remaining;
                        current_index ++;
                        bytes_to_read_in_cycle -= bytes_remaining;
                        bytes_remaining = 0;
                        continue;
                    }
                    else {
                        bytes_per_process[n] += bytes_to_read_in_cycle;
                        bytes_remaining -= bytes_to_read_in_cycle;
                        bytes_to_read_in_cycle = 0;
                        break;
                    }
                }
                else {
                    if (bytes_to_read_in_cycle < 
                        global_fview[sorted[current_index]].iov_len) {
                        bytes_per_process[n] += bytes_to_read_in_cycle;
                        bytes_remaining = 
                            global_fview[sorted[current_index]].iov_len - 
                            bytes_to_read_in_cycle;
                        bytes_to_read_in_cycle = 0;
                        break;
                    }
                    else {
                        bytes_per_process[n] += 
                            global_fview[sorted[current_index]].iov_len;
                        bytes_to_read_in_cycle -= 
                            global_fview[sorted[current_index]].iov_len;
                        current_index ++;
                        continue;
                    }
                }
            }
            /*
            for (i=0 ; i<fh->f_size ; i++) {
                printf ("%d --> expecting %d from %d\n",fh->f_rank, 
                        bytes_per_process[i], i);
            }
            */
            /* Calculate the displacement on where to put the data and allocate
               the recieve buffer (global_buf) */
            displs[0] = 0;
            global_count = bytes_per_process[0];
            for (i=1 ; i<fh->f_size ; i++) {
                global_count += bytes_per_process[i];
                displs[i] = displs[i-1] + bytes_per_process[i-1];
            }
            /*
            for (i=0 ; i<fh->f_size ; i++) {
                printf ("Proc %d sending %d at %d\n",
                        i,
                        bytes_per_process[i],
                        displs[i]);
            }
            */
            if (0 != global_count) {
                global_buf = malloc (global_count);
                if (NULL == global_buf) {
                    opal_output (1, "OUT OF MEMORY\n");
                    return OMPI_ERR_OUT_OF_RESOURCE;
                }
            }
        }
        /**********************************************************
         ******* Create the io array, and pass it to fbtl *********
         *********************************************************/
        if (0 == fh->f_rank%fh->f_aggregator_index && global_count) {
            int bytes_to_read = global_count;
            int *temp = NULL;
            int block = 1;
            k = 0;

            temp = (int *)malloc (sizeof(int) * fh->f_size);
            if (NULL == temp) {
                opal_output(1, "OUT OF MEMORY\n");
                return OMPI_ERR_OUT_OF_RESOURCE;
            }
            memset(temp, 0x0, fh->f_size*sizeof(int));
            fh->f_io_array = (mca_io_ompio_io_array_t *) malloc 
                (OMPIO_IOVEC_INITIAL_SIZE * sizeof (mca_io_ompio_io_array_t));
            if (NULL == fh->f_io_array) {
                opal_output(1, "OUT OF MEMORY\n");
                return OMPI_ERR_OUT_OF_RESOURCE;
            }
            
            while (bytes_to_read) {
                int start = 0;

                if (OMPIO_IOVEC_INITIAL_SIZE*block <= k) {
                    block ++;
                    fh->f_io_array = (mca_io_ompio_io_array_t *)realloc
                        (fh->f_io_array, OMPIO_IOVEC_INITIAL_SIZE * block *
                         sizeof(mca_io_ompio_io_array_t));
                    if (NULL == fh->f_io_array) {
                        opal_output(1, "OUT OF MEMORY\n");
                        return OMPI_ERR_OUT_OF_RESOURCE;
                    }
                }

                blocks = fview_count[0];
                for (j=0 ; j<fh->f_size ; j++) {
                    if (sorted[x] < blocks) {
                        n = j;
                        break;
                    }
                    else {
                        blocks += fview_count[j+1];
                    }
                }
                for (j=0 ; j<n ; j++) {
                    start += bytes_per_process[j];
                }
 
                if (bytes_left) {
                    if (bytes_left <= bytes_to_read) {
                        fh->f_io_array[k].offset = (IOVBASE_TYPE *)
                            ((OPAL_PTRDIFF_TYPE)global_fview[sorted[x]].iov_base + 
                             (global_fview[sorted[x]].iov_len - bytes_left));
                        fh->f_io_array[k].length = bytes_left;
                        fh->f_io_array[k].memory_address = 
                            &global_buf[start+temp[n]];
                        temp[n] += (int)fh->f_io_array[k].length;
                        bytes_to_read -= bytes_left;
                        bytes_left = 0;
                        k ++;
                        x ++;
                        continue;
                    }
                    else {
                        fh->f_io_array[k].offset = (IOVBASE_TYPE *) 
                            ((OPAL_PTRDIFF_TYPE)global_fview[sorted[x]].iov_base + 
                             (global_fview[sorted[x]].iov_len - bytes_left));
                        fh->f_io_array[k].length = bytes_to_read;
                        fh->f_io_array[k].memory_address = 
                            &global_buf[start+temp[n]];
                        temp[n] += (int)fh->f_io_array[k].length;
                        bytes_left -= bytes_to_read;
                        bytes_to_read = 0;;
                        k ++;
                        break;
                    }
                }
                else {
                    if (bytes_to_read < (int)global_fview[sorted[x]].iov_len) {
                        fh->f_io_array[k].offset = global_fview[sorted[x]].iov_base;
                        fh->f_io_array[k].length = bytes_to_read;
                        fh->f_io_array[k].memory_address = 
                            &global_buf[start+temp[n]];
                        bytes_left = 
                            global_fview[sorted[x]].iov_len - bytes_to_read;
                        bytes_to_read = 0;
                        k ++;
                        break;
                    }
                    else {
                        fh->f_io_array[k].offset = global_fview[sorted[x]].iov_base;
                        fh->f_io_array[k].length = global_fview[sorted[x]].iov_len;
                        fh->f_io_array[k].memory_address = 
                            &global_buf[start+temp[n]];
                        temp[n] += (int)fh->f_io_array[k].length;
                        bytes_to_read -= global_fview[sorted[x]].iov_len;
                        k ++;
                        x ++;
                        continue;
                    }
                }
            }

            fh->f_num_of_io_entries = k;
            /*
            printf("*************************** %d\n", fh->f_num_of_io_entries);
            for (i=0 ; i<fh->f_num_of_io_entries ; i++) {
                printf(" ADDRESS: %p  OFFSET: %p   LENGTH: %d\n",
                       fh->f_io_array[i].memory_address,
                       fh->f_io_array[i].offset,
                       fh->f_io_array[i].length);
            }
            */
            if (fh->f_num_of_io_entries) {
                if (OMPI_SUCCESS != fh->f_fbtl->fbtl_preadv (fh, NULL)) {
                    opal_output (1, "READ FAILED\n");
                    return OMPI_ERROR;
                }
            }
            if (NULL != temp) {
                free (temp);
                temp = NULL;
            }
        }
        /**********************************************************
         ******************** DONE READING ************************
         *********************************************************/


        /**********************************************************
         ********* Scatter the Data from the readers **************
         *********************************************************/
#if 0
        if (fh->f_flags & OMPIO_CONTIGUOUS_MEMORY) {
            receive_buf = &((char*)buf)[total_bytes_read];
        }
        else if (total_bytes_recv) {
            /* allocate a send buffer and copy the data that needs
               to be sent into it in case the data is non-contigous
               in memory */
            receive_buf = malloc (total_bytes_recv);
            if (NULL == receive_buf) {
                opal_output (1, "OUT OF MEMORY\n");
                return OMPI_ERR_OUT_OF_RESOURCE;
            }
        }
#endif
        /* distribute the data to its corresponding processes */
        ompi_io_ompio_receive_data (fh,
                                    buf,
                                    total_bytes_recv,
                                    decoded_iov,
                                    iov_count,
                                    bytes_received,
                                    broken_iovec,
                                    previous,
                                    prev_bytes_rem,
                                    global_buf,
                                    bytes_per_process,
                                    displs,
                                    two_phase_num_io_procs,
                                    stripe_size);

        /*
        if (0 == fh->f_rank%fh->f_aggregator_index) {
            for (k=0 ; k<global_count/4 ; k++) {
                printf ("%d: RECV %d \n",fh->f_rank,
                        ((int *)global_buf)[k]);
            }
        }
        */
#if 0
        /* If data is not contigous in memory, copy the data from the 
           receive buffer into the buffer passed in */
        if (!(fh->f_flags & OMPIO_CONTIGUOUS_MEMORY)) {
            OPAL_PTRDIFF_TYPE mem_address;
            size_t remaining = 0;
            size_t temp_position = 0;

            remaining = total_bytes_recv;

            while (remaining) {
                mem_address = (OPAL_PTRDIFF_TYPE)
                    (decoded_iov[iov_index].iov_base) + current_position;

                if (remaining >= 
                    (decoded_iov[iov_index].iov_len - current_position)) {
                    memcpy ((IOVBASE_TYPE *) mem_address,
                            receive_buf+temp_position,
                            decoded_iov[iov_index].iov_len - current_position);
                    remaining = remaining - 
                        (decoded_iov[iov_index].iov_len - current_position);
                    temp_position = temp_position +
                        (decoded_iov[iov_index].iov_len - current_position);
                    iov_index = iov_index + 1;
                    current_position = 0;
                }
                else {
                    memcpy ((IOVBASE_TYPE *) mem_address,
                            receive_buf+temp_position,
                            remaining);
                    current_position = current_position + remaining;
                    remaining = 0;
                }
            }
            if (NULL != receive_buf) {
                free (receive_buf);
                receive_buf = NULL;
            }
        }
#endif
        total_bytes_read += total_bytes_recv;
        total_bytes -= global_count;

        /**********************************************************
         **************** DONE GATHERING OF DATA ******************
         *********************************************************/



        if (0 == fh->f_rank%fh->f_aggregator_index) {
            fh->f_num_of_io_entries = 0;
            if (NULL != fh->f_io_array) {
                free (fh->f_io_array);
                fh->f_io_array = NULL;
            }
            if (NULL != global_buf) {
                free (global_buf);
                global_buf = NULL;
            }
        }
    }
    if (NULL != sorted) {
        free (sorted);
        sorted = NULL;
    }
    if (NULL != broken_iovec) {
        free (broken_iovec);
        broken_iovec = NULL;
    }
    if (NULL != global_fview) {
        free (global_fview);
        global_fview = NULL;
    }
    if (NULL != fview_count) {
        free (fview_count);
        fview_count = NULL;
    }
    if (NULL != decoded_iov) {
        free (decoded_iov);
        decoded_iov = NULL;
    }
    if (NULL != bytes_per_process) {
        free (bytes_per_process);
        bytes_per_process = NULL;
    }
    if (NULL != bytes_received) {
        free (bytes_received);
        bytes_received = NULL;
    }
    if (NULL != displs) {
        free (displs);
        displs = NULL;
    }
    /*
    if (NULL != total_bytes_per_process) {
        free (total_bytes_per_process);
        total_bytes_per_process = NULL;
    }
    */

    fh->f_flags ^= OMPIO_AGGREGATOR_IS_SET;
    return OMPI_SUCCESS;
}
