/*
 * pkcs11_objects.c
 *
 * Copyright (C) STMicroelectronics SA 2018
 * Author: etienne carriere <etienne.carriere@st.com> for STMicroelectronics.
 */



CK_RV ck_find_objects_init(CK_SESSION_HANDLE session,
			   CK_ATTRIBUTE_PTR attribs,
			   CK_ULONG count)
{
	CK_RV rv;
	uint32_t session_handle = session;
	struct serializer obj;
	char *ctrl;
	size_t ctrl_size;

	rv = serialize_ck_attributes(&obj, attribs, count);
	if (rv)
		return rv;

	/* ctrl = [session-handle][headed-serialized-attributes] */
	ctrl_size = sizeof(uint32_t) + obj.size;
	ctrl = malloc(ctrl_size);
	if (!ctrl) {
		rv = CKR_HOST_MEMORY;
		goto bail;
	}

	memcpy(ctrl, &session_handle, sizeof(uint32_t));
	memcpy(ctrl + sizeof(uint32_t), obj.buffer, obj.size);

	rv = ck_invoke_ta(ck_session2sks_ctx(session),
			  SKS_CMD_FIND_OBJECTS_INIT, ctrl, ctrl_size,
			  NULL, 0, NULL, NULL);

bail:
	release_serial_object(&obj);
	free(ctrl);
	return rv;
}

CK_RV ck_find_objects(CK_SESSION_HANDLE session,
			CK_OBJECT_HANDLE_PTR obj,
			CK_ULONG max_count,
			CK_ULONG_PTR count)

{
	CK_RV rv;
	uint32_t ctrl[2] = { session, max_count };
	uint32_t *handles;
	size_t handles_size = max_count * sizeof(uint32_t);
	CK_ULONG n;
	CK_ULONG last;

	handles = malloc(handles_size);
	if (!handles)
		return CKR_HOST_MEMORY;

	rv = ck_invoke_ta(ck_session2sks_ctx(session),
			  SKS_CMD_FIND_OBJECTS, ctrl, sizeof(ctrl),
			  NULL, 0, handles, &handles_size);

	if (rv)
		goto bail;

	last = handles_size / sizeof(uint32_t);
	*count = last;

	for (n = 0; n < last; n++) {
		obj[n] = handles[n];
	}

bail:
	free(handles);
	return rv;

}

CK_RV ck_find_objects_final(CK_SESSION_HANDLE session)
{
	CK_RV rv;
	uint32_t ctrl[1] = { session };

	rv = ck_invoke_ta(ck_session2sks_ctx(session),
			  SKS_CMD_FIND_OBJECTS_FINAL, ctrl, sizeof(ctrl),
			  NULL, 0, NULL, NULL);

	return rv;
}


