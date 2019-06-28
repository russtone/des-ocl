#ifdef __APPLE__
/* Ignore annoying ssl deprecated warnings */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "OpenCL/opencl.h"
#else
#include "CL/opencl.h"
#endif

#include <openssl/des.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CL_WRAPPER(FUNC) {                                   \
    cl_int error = FUNC;                                     \
    if (error != CL_SUCCESS) {                               \
	fprintf(stderr, "Error %d executing %s on %s:%d\n",  \
	error, #FUNC, __FILE__, __LINE__);                   \
	abort();                                             \
    };                                                       \
}

#define CL_ASSIGN(ASSIGNMENT) {                              \
    cl_int error;                                            \
    ASSIGNMENT;                                              \
    if (error != CL_SUCCESS) {                               \
	fprintf(stderr, "Error %d executing %s on %s:%d\n",  \
	error, #ASSIGNMENT, __FILE__, __LINE__);             \
	abort();                                             \
    };                                                       \
}

#define MAX_SOURCE_SIZE 100000
#define BLOCK_SIZE_OPTIMAL 256

int main()
{
    FILE *fp;
    char fileName[] = "./des_brute_kernel.cl";
    char *source_str;
    size_t source_size;

    /* Load the source code containing the kernel */
    fp = fopen(fileName, "r");
    if (!fp) {
	fprintf(stderr, "Failed to load kernel.\n");
	abort();
    }

    source_str = (char*)malloc(MAX_SOURCE_SIZE);
    source_size = fread(source_str, 1, MAX_SOURCE_SIZE, fp);
    fclose(fp);


    /* Get Platform and Device Info */
    cl_platform_id platform_id;
    cl_uint ret_num_platforms;
    CL_WRAPPER(clGetPlatformIDs(1, &platform_id, &ret_num_platforms));

    cl_device_id device_id;
    cl_uint ret_num_devices;
    CL_WRAPPER(clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1, &device_id, &ret_num_devices));

    /* Create OpenCL context */
    cl_context context;
    CL_ASSIGN(context = clCreateContext(NULL, 1, &device_id, NULL, NULL, &error));

    /* Create Command Queue */
    cl_command_queue command_queue;
    CL_ASSIGN(command_queue = clCreateCommandQueue(context, device_id, 0, &error));

    /* Create Kernel Program from the source */
    cl_program program;
    CL_ASSIGN(program = clCreateProgramWithSource(context, 1, (const char **)&source_str,
	(const size_t *)&source_size, &error));

    /* Build Kernel Program */
    if (clBuildProgram(program, 1, &device_id, NULL, NULL, NULL) != CL_SUCCESS) {
	char build_log[10240] = { 0 };
	clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, sizeof(build_log), build_log, NULL);
	fprintf(stderr, "CL Compilation failed:\n%s", build_log);
	abort();
    }

    /* Create DES encrypt kernel */
    cl_kernel kernel;
    CL_ASSIGN(kernel = clCreateKernel(program, "des_brute_kernel", &error));

    /* Create device buffers */
    cl_mem device_key;
    CL_ASSIGN(device_key = clCreateBuffer(context, CL_MEM_READ_ONLY, 8, NULL, &error))

    cl_mem device_plain;
    CL_ASSIGN(device_plain = clCreateBuffer(context, CL_MEM_READ_ONLY, 8, NULL, &error))

    cl_mem device_cipher;
    CL_ASSIGN(device_cipher = clCreateBuffer(context, CL_MEM_READ_ONLY, 8, NULL, &error))

    cl_mem device_result;
    CL_ASSIGN(device_result = clCreateBuffer(context, CL_MEM_READ_WRITE, 8, NULL, &error))

    /* Set kernel args */
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &device_key);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &device_plain);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &device_cipher);
    clSetKernelArg(kernel, 3, sizeof(cl_mem), &device_result);

    /* Test */
    DES_cblock key, key_orig, key_found, plain, cipher;
    DES_key_schedule schedule;

    /* Gen randow DES key */
    DES_random_key(&key);

    /* Save key for later cheching */
    memcpy(&key_orig, &key, 8);
    DES_set_key(&key, &schedule);

    /* Gen random block and encrypt it */
    RAND_bytes((unsigned char *)&plain, 8);
    DES_ecb_encrypt(&plain, &cipher, &schedule, DES_ENCRYPT);

    /* Copy data to device */
    /* For test we will brute last two bytes of key so zero them */
    key[0] = 0;
    key[1] = 0;
    CL_WRAPPER(clEnqueueWriteBuffer(command_queue, device_key, CL_TRUE, 0, 8, key, 0, NULL, NULL));
    CL_WRAPPER(clEnqueueWriteBuffer(command_queue, device_plain, CL_TRUE, 0, 8, plain, 0, NULL, NULL));
    CL_WRAPPER(clEnqueueWriteBuffer(command_queue, device_cipher, CL_TRUE, 0, 8, cipher, 0, NULL, NULL));

    /* Get max grid size */
    size_t max_grid_size[3];
    CL_WRAPPER(clGetDeviceInfo(device_id, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(max_grid_size), &max_grid_size, NULL));
    /* size_t max_one_dim_size = max_grid_size[0] * max_grid_size[1] * max_grid_size[2]; */

    /* Run kernel */
    cl_event kernel_completion;
    size_t block_size[3] = { BLOCK_SIZE_OPTIMAL, 0, 0 };

    /* two bytes = 2^16 = 65536, so grid size should be 65536 */
    size_t grid_size[3] = { 65536, 0, 0 };
    CL_WRAPPER(clEnqueueNDRangeKernel(command_queue, kernel, 1, NULL, grid_size, block_size, 0, NULL, &kernel_completion));
    CL_WRAPPER(clWaitForEvents(1, &kernel_completion));

    /* Get result */
    CL_WRAPPER(clEnqueueReadBuffer(command_queue, device_result, CL_TRUE, 0, 8, key_found, 0, NULL, NULL));

    /* Check result */

    /* Set parity bits in found key */
    DES_set_odd_parity(&key_found);

    if (memcmp(key_found, key_orig, sizeof(DES_cblock)) == 0) {
	printf("Success\n");
    }
    else {
	printf("Fail\n");
    }


    /* Finalization */
    CL_WRAPPER(clFlush(command_queue));
    CL_WRAPPER(clFinish(command_queue));
    CL_WRAPPER(clReleaseKernel(kernel));
    CL_WRAPPER(clReleaseProgram(program));
    CL_WRAPPER(clReleaseMemObject(device_key));
    CL_WRAPPER(clReleaseMemObject(device_plain));
    CL_WRAPPER(clReleaseMemObject(device_cipher));
    CL_WRAPPER(clReleaseMemObject(device_result));
    CL_WRAPPER(clReleaseCommandQueue(command_queue));
    CL_WRAPPER(clReleaseContext(context));

    free(source_str);

    return 0;
}

