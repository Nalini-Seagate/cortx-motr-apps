/* -*- C -*- */
/*
 * COPYRIGHT 2014 SEAGATE LLC
 *
 * THIS DRAWING/DOCUMENT, ITS SPECIFICATIONS, AND THE DATA CONTAINED
 * HEREIN, ARE THE EXCLUSIVE PROPERTY OF SEAGATE LLC,
 * ISSUED IN STRICT CONFIDENCE AND SHALL NOT, WITHOUT
 * THE PRIOR WRITTEN PERMISSION OF SEAGATE TECHNOLOGY LIMITED,
 * BE REPRODUCED, COPIED, OR DISCLOSED TO A THIRD PARTY, OR
 * USED FOR ANY PURPOSE WHATSOEVER, OR STORED IN A RETRIEVAL SYSTEM
 * EXCEPT AS ALLOWED BY THE TERMS OF SEAGATE LICENSES AND AGREEMENTS.
 *
 * YOU SHOULD HAVE RECEIVED A COPY OF SEAGATE'S LICENSE ALONG WITH
 * THIS RELEASE. IF NOT PLEASE CONTACT A SEAGATE REPRESENTATIVE
 * http://www.xyratex.com/contact
 *
 * Original author:  Ganesan Umanesan <ganesan.umanesan@seagate.com>
 * Original creation date: 10-Jan-2017
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <assert.h>

#include "clovis/clovis.h"
#include "clovis/clovis_idx.h"

#ifndef DEBUG
#define DEBUG 0
#endif

#define MAXCHAR 256
#define C0RCFLE "./.cappsrc"
char c0rc[8][MAXCHAR];
#define CLOVIS_MAX_BLOCK_COUNT (200)

/* static variables */
static struct m0_clovis          *clovis_instance = NULL;
static struct m0_clovis_container clovis_container;
static struct m0_clovis_realm     clovis_uber_realm;
static struct m0_clovis_config    clovis_conf;


/*
 * open_entity()
 * open clovis entity.
 */
static int open_entity(struct m0_clovis_entity *entity)
{
	int                                 rc;
	struct m0_clovis_op *ops[1] = {NULL};

	m0_clovis_entity_open(entity, &ops[0]);
	m0_clovis_op_launch(ops, 1);
	m0_clovis_op_wait(ops[0], M0_BITS(M0_CLOVIS_OS_FAILED,
			M0_CLOVIS_OS_STABLE),
			M0_TIME_NEVER);
	rc = m0_clovis_rc(ops[0]);
	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	ops[0] = NULL;

	return rc;
}

/*
 * create_object()
 * create clovis object.
 */
static int create_object(struct m0_uint128 id)
{
	int                  rc;
	struct m0_clovis_obj obj;
	struct m0_clovis_op *ops[1] = {NULL};

	memset(&obj, 0, sizeof(struct m0_clovis_obj));

	m0_clovis_obj_init(&obj, &clovis_uber_realm, &id,
			   m0_clovis_default_layout_id(clovis_instance));

	rc = open_entity(&obj.ob_entity);
	fprintf(stderr,"error! [%d]\n", rc);
	if (rc > 0) {
		fprintf(stderr,"error! [%d]\n", rc);
		fprintf(stderr,"object already exists\n");
		return rc;
	}

	m0_clovis_entity_create(&obj.ob_entity, &ops[0]);

	m0_clovis_op_launch(ops, ARRAY_SIZE(ops));

	rc = m0_clovis_op_wait(
		ops[0], M0_BITS(M0_CLOVIS_OS_FAILED, M0_CLOVIS_OS_STABLE),
		m0_time_from_now(3,0));

	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	m0_clovis_entity_fini(&obj.ob_entity);

	return 0;
}

static int read_data_from_file(FILE *fp, struct m0_bufvec *data, int bsz)
{
	int i;
	int rc;
	int nr_blocks;

	nr_blocks = data->ov_vec.v_nr;
	for (i = 0; i < nr_blocks; i++) {
		rc = fread(data->ov_buf[i], bsz, 1, fp);
		if (rc != 1)
			break;

		if (feof(fp))
			break;
	}

	return i;
}

static int write_data_to_object(struct m0_uint128 id,
				struct m0_indexvec *ext,
				struct m0_bufvec *data,
				struct m0_bufvec *attr)
{
	int                  rc;
	struct m0_clovis_obj obj;
	struct m0_clovis_op *ops[1] = {NULL};

	memset(&obj, 0, sizeof(struct m0_clovis_obj));

	/* Set the object entity we want to write */
	m0_clovis_obj_init(&obj, &clovis_uber_realm, &id,
			   m0_clovis_default_layout_id(clovis_instance));

	open_entity(&obj.ob_entity);

	/* Create the write request */
	m0_clovis_obj_op(&obj, M0_CLOVIS_OC_WRITE,
			 ext, data, attr, 0, &ops[0]);

	/* Launch the write request*/
	m0_clovis_op_launch(ops, 1);

	/* wait */
	rc = m0_clovis_op_wait(ops[0],
			M0_BITS(M0_CLOVIS_OS_FAILED,
				M0_CLOVIS_OS_STABLE),
			M0_TIME_NEVER);

	/* fini and release */
	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	m0_clovis_entity_fini(&obj.ob_entity);

	return rc;
}


/*
 * objcpy()
 * copy to an object.
 */
int objcpy(int64_t idhi, int64_t idlo, char *filename, int bsz, int cnt)
{
	int                i;
	int                rc;
	int                block_count;
	uint64_t           last_index;
	struct m0_uint128  id;
	struct m0_indexvec ext;
	struct m0_bufvec   data;
	struct m0_bufvec   attr;
	FILE              *fp;

	/* open src file */
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        fprintf(stderr,"error! could not open input file %s\n",filename);
        return 1;
    }

	/* ids */
	id.u_hi = idhi;
	id.u_lo = idlo;

    /* create object */
	rc = create_object(id);
	if (rc != 0) {
		fprintf(stderr, "Can't create object!\n");
		fclose(fp);
		return rc;
	}

	last_index = 0;
	while (cnt > 0) {
		block_count = (cnt > CLOVIS_MAX_BLOCK_COUNT)?
			      CLOVIS_MAX_BLOCK_COUNT:cnt;

		/* Allocate block_count * 4K data buffer. */
		rc = m0_bufvec_alloc(&data, block_count, bsz);
		if (rc != 0)
			return rc;

		/* Allocate bufvec and indexvec for write. */
		rc = m0_bufvec_alloc(&attr, block_count, 1);
		if(rc != 0)
			return rc;

		rc = m0_indexvec_alloc(&ext, block_count);
		if (rc != 0)
			return rc;

		/*
		 * Prepare indexvec for write: <clovis_block_count> from the
		 * beginning of the object.
		 */
		for (i = 0; i < block_count; i++) {
			ext.iv_index[i] = last_index ;
			ext.iv_vec.v_count[i] = bsz;
			last_index += bsz;

			/* we don't want any attributes */
			attr.ov_vec.v_count[i] = 0;
		}

		/* Read data from source file. */
		rc = read_data_from_file(fp, &data, bsz);
		assert(rc == block_count);

		/* Copy data to the object*/
		rc = write_data_to_object(id, &ext, &data, &attr);
		if (rc != 0) {
			fprintf(stderr, "Writing to object failed!\n");
			return rc;
		}

		/* Free bufvec's and indexvec's */
		m0_indexvec_free(&ext);
		m0_bufvec_free(&data);
		m0_bufvec_free(&attr);

		cnt -= block_count;
	}

    fclose(fp);
    return 0;
}

/*
 * objcat()
 * cat object.
 */
int objcat(int64_t idhi, int64_t idlo, int bsz, int cnt)
{
	int                     i;
	int                     j;
	int                     rc;
	struct m0_uint128       id;
	struct m0_clovis_op    *ops[1] = {NULL};
	struct m0_clovis_obj    obj;
	uint64_t                last_index;
	struct m0_indexvec      ext;
	struct m0_bufvec        data;
	struct m0_bufvec        attr;

	/* ids */
	id.u_hi = idhi;
	id.u_lo = idlo;

	/* we want to read <clovis_block_count> from the beginning of the object */
	rc = m0_indexvec_alloc(&ext, cnt);
	if (rc != 0)
		return rc;

	/*
	 * this allocates <clovis_block_count> * 4K buffers for data, and initialises
	 * the bufvec for us.
	 */
	rc = m0_bufvec_alloc(&data, cnt, bsz);
	if (rc != 0)
		return rc;
	rc = m0_bufvec_alloc(&attr, cnt, 1);
	if(rc != 0)
		return rc;

	last_index = 0;
	for (i = 0; i < cnt; i++) {
		ext.iv_index[i] = last_index ;
		ext.iv_vec.v_count[i] = bsz;
		last_index += bsz;

		/* we don't want any attributes */
		attr.ov_vec.v_count[i] = 0;

	}

	M0_SET0(&obj);
	/* Read the requisite number of blocks from the entity */
	m0_clovis_obj_init(&obj, &clovis_uber_realm, &id,
			   m0_clovis_default_layout_id(clovis_instance));

	/* open entity */
	rc = open_entity(&obj.ob_entity);
	if (rc < 0) {
		fprintf(stderr,"error! [%d]\n", rc);
		fprintf(stderr,"object not found\n");
		return rc;
	}


	/* Create the read request */
	m0_clovis_obj_op(&obj, M0_CLOVIS_OC_READ, &ext, &data, &attr, 0, &ops[0]);
	assert(rc == 0);
	assert(ops[0] != NULL);
	assert(ops[0]->op_sm.sm_rc == 0);

	m0_clovis_op_launch(ops, 1);

	/* wait */
	rc = m0_clovis_op_wait(ops[0],
			    M0_BITS(M0_CLOVIS_OS_FAILED,
				    M0_CLOVIS_OS_STABLE),
		     M0_TIME_NEVER);
	assert(rc == 0);
	assert(ops[0]->op_sm.sm_state == M0_CLOVIS_OS_STABLE);
	assert(ops[0]->op_sm.sm_rc == 0);

	/* putchar the output */
	for (i = 0; i < cnt; i++) {
		for(j = 0; j < data.ov_vec.v_count[i]; j++) {
			putchar(((char *)data.ov_buf[i])[j]);
		}
	}

	/* fini and release */
	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	m0_clovis_entity_fini(&obj.ob_entity);

	m0_indexvec_free(&ext);
	m0_bufvec_free(&data);
	m0_bufvec_free(&attr);

	return 0;
}


/*
 * objdel()
 * delete object.
 */
int objdel(int64_t idhi, int64_t idlo)
{
	int rc;
	struct m0_clovis_obj obj;
	struct m0_clovis_op *ops[1] = {NULL};
	struct m0_uint128 id;

	id.u_hi = idhi;
	id.u_lo = idlo;

	memset(&obj, 0, sizeof(struct m0_clovis_obj));
	m0_clovis_obj_init(&obj, &clovis_uber_realm, &id,
			   m0_clovis_default_layout_id(clovis_instance));
	rc = open_entity(&obj.ob_entity);
	if (rc < 0) {
		fprintf(stderr,"error! [%d]\n", rc);
		fprintf(stderr,"object not found\n");
		return rc;
	}

	m0_clovis_entity_delete(&obj.ob_entity, &ops[0]);
	m0_clovis_op_launch(ops, ARRAY_SIZE(ops));
	rc = m0_clovis_op_wait(
		ops[0], M0_BITS(M0_CLOVIS_OS_FAILED, M0_CLOVIS_OS_STABLE),
		m0_time_from_now(3,0));

	m0_clovis_op_fini(ops[0]);
	m0_clovis_op_free(ops[0]);
	m0_clovis_entity_fini(&obj.ob_entity);

	return rc;
}



/*
 * c0init()
 * init clovis resources.
 */
int c0init(void)
{
	int rc;
    FILE *fp;
    char str[MAXCHAR];
    char* filename = C0RCFLE;
    int i;

	/* read c0rc file */
    fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr,"error! could not open resource file %s\n",filename);
        return 1;
    }

    i = 0;
    while (fgets(str, MAXCHAR, fp) != NULL) {
    	str[strlen(str) - 1] = '\0';
    	memset(c0rc[i], 0x00, MAXCHAR);
    	strncpy(c0rc[i], str, MAXCHAR);

#if DEBUG
    	fprintf(stderr,"%s", str);
    	fprintf(stderr,"%s", c0rc[i]);
    	fprintf(stderr,"\n");
#endif

    	i++;
    	if(i==8) break;
    }
    fclose(fp);

	clovis_conf.cc_is_oostore            = false;
	clovis_conf.cc_is_read_verify        = false;
	clovis_conf.cc_local_addr            = c0rc[0];	/* clovis_local_addr	*/
	clovis_conf.cc_ha_addr               = c0rc[1];	/* clovis_ha_addr		*/
	clovis_conf.cc_profile               = c0rc[2];	/* clovis_prof			*/
	clovis_conf.cc_process_fid           = c0rc[3];	/* clovis_proc_fid		*/
	clovis_conf.cc_idx_service_conf      = c0rc[4]; /* clovis_index_dir		*/
	clovis_conf.cc_tm_recv_queue_min_len = 16;
	clovis_conf.cc_max_rpc_msg_size      = M0_RPC_DEF_MAX_RPC_MSG_SIZE;
	clovis_conf.cc_idx_service_id        = M0_CLOVIS_IDX_MOCK;
	clovis_conf.cc_layout_id 	     	 = 0;

#if DEBUG
	fprintf(stderr,"---\n");
    fprintf(stderr,"%s", (char *)clovis_conf.cc_local_addr);
    fprintf(stderr,"%s", (char *)clovis_conf.cc_ha_addr);
    fprintf(stderr,"%s", (char *)clovis_conf.cc_profile);
    fprintf(stderr,"%s", (char *)clovis_conf.cc_process_fid);
    fprintf(stderr,"%s", (char *)clovis_conf.cc_idx_service_conf);
	fprintf(stderr,"---\n");
#endif

	/* clovis instance */
	rc = m0_clovis_init(&clovis_instance, &clovis_conf, true);
	if (rc != 0) {
		fprintf(stderr,"Failed to initilise Clovis\n");
		return rc;
	}

	/* And finally, clovis root realm */
	m0_clovis_container_init(&clovis_container, 
				 NULL, &M0_CLOVIS_UBER_REALM,
				 clovis_instance);
	rc = clovis_container.co_realm.re_entity.en_sm.sm_rc;
	if (rc != 0) {
		fprintf(stderr,"Failed to open uber realm\n");
		return rc;
	}

	/* success */
	clovis_uber_realm = clovis_container.co_realm;
	return 0;
}

/*
 * c0free()
 * free clovis resources.
 */
void c0free(void)
{
	m0_clovis_fini(clovis_instance, true);
	return;
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */