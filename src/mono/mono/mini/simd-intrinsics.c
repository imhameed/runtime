/**
 * SIMD Intrinsics support for netcore.
 * Only LLVM is supported as a backend.
 */

#include <config.h>
#include <mono/utils/mono-compiler.h>
#include <mono/metadata/icall-decl.h>
#include "mini.h"
#include "mini-runtime.h"
#include "ir-emit.h"
#ifdef ENABLE_LLVM
#include "mini-llvm.h"
#include "mini-llvm-cpp.h"
#endif
#include "mono/utils/bsearch.h"
#include <mono/metadata/abi-details.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/utils/mono-hwcap.h>

#if defined (MONO_ARCH_SIMD_INTRINSICS)

#if defined(DISABLE_JIT)

void
mono_simd_intrinsics_init (void)
{
}

#else

#define MSGSTRFIELD(line) MSGSTRFIELD1(line)
#define MSGSTRFIELD1(line) str##line
static const struct msgstr_t {
#define METHOD(name) char MSGSTRFIELD(__LINE__) [sizeof (#name)];
#define METHOD2(str,name) char MSGSTRFIELD(__LINE__) [sizeof (str)];
#include "simd-methods.h"
#undef METHOD
#undef METHOD2
} method_names = {
#define METHOD(name) #name,
#define METHOD2(str,name) str,
#include "simd-methods.h"
#undef METHOD
#undef METHOD2
};

enum {
#define METHOD(name) SN_ ## name = offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#define METHOD2(str,name) SN_ ## name = offsetof (struct msgstr_t, MSGSTRFIELD(__LINE__)),
#include "simd-methods.h"
};
#define method_name(idx) ((const char*)&method_names + (idx))

static int register_size;

#define None 0

typedef struct {
	uint16_t id; // One of the SN_ constants
	uint16_t default_op; // ins->opcode
	uint16_t default_instc0; // ins->inst_c0
	uint16_t unsigned_op;
	uint16_t unsigned_instc0;
	uint16_t floating_op;
	uint16_t floating_instc0;
} SimdIntrinsic;

static const SimdIntrinsic unsupported [] = { {SN_get_IsSupported} };

void
mono_simd_intrinsics_init (void)
{
	register_size = 16;
#if FALSE
	if ((mini_get_cpu_features () & MONO_CPU_X86_AVX) != 0)
		register_size = 32;
#endif
	/* Tell the class init code the size of the System.Numerics.Register type */
	mono_simd_register_size = register_size;
}

MonoInst*
mono_emit_simd_field_load (MonoCompile *cfg, MonoClassField *field, MonoInst *addr)
{
	return NULL;
}

static int
simd_intrinsic_compare_by_name (const void *key, const void *value)
{
	return strcmp ((const char*)key, method_name (*(guint16*)value));
}

static int
simd_intrinsic_info_compare_by_name (const void *key, const void *value)
{
	SimdIntrinsic *info = (SimdIntrinsic*)value;
	return strcmp ((const char*)key, method_name (info->id));
}

static int
lookup_intrins (guint16 *intrinsics, int size, MonoMethod *cmethod)
{
	const guint16 *result = (const guint16 *)mono_binary_search (cmethod->name, intrinsics, size / sizeof (guint16), sizeof (guint16), &simd_intrinsic_compare_by_name);

	if (result == NULL)
		return -1;
	else
		return (int)*result;
}

static SimdIntrinsic*
lookup_intrins_info (SimdIntrinsic *intrinsics, int size, MonoMethod *cmethod)
{
#if 0
	for (int i = 0; i < (size / sizeof (SimdIntrinsic)) - 1; ++i) {
		const char *n1 = method_name (intrinsics [i].id);
		const char *n2 = method_name (intrinsics [i + 1].id);
		int len1 = strlen (n1);
		int len2 = strlen (n2);
		for (int j = 0; j < len1 && j < len2; ++j) {
			if (n1 [j] > n2 [j]) {
				printf ("%s %s\n", n1, n2);
				g_assert_not_reached ();
			} else if (n1 [j] < n2 [j]) {
				break;
			}
		}
	}
#endif

	return (SimdIntrinsic *)mono_binary_search (cmethod->name, intrinsics, size / sizeof (SimdIntrinsic), sizeof (SimdIntrinsic), &simd_intrinsic_info_compare_by_name);
}

/*
 * Return a simd vreg for the simd value represented by SRC.
 * SRC is the 'this' argument to methods.
 * Set INDIRECT to TRUE if the value was loaded from memory.
 */
static int
load_simd_vreg_class (MonoCompile *cfg, MonoClass *klass, MonoInst *src, gboolean *indirect)
{
	const char *spec = INS_INFO (src->opcode);

	if (indirect)
		*indirect = FALSE;
	if (src->opcode == OP_XMOVE) {
		return src->sreg1;
	} else if (src->opcode == OP_LDADDR) {
		int res = ((MonoInst*)src->inst_p0)->dreg;
		return res;
	} else if (spec [MONO_INST_DEST] == 'x') {
		return src->dreg;
	} else if (src->type == STACK_PTR || src->type == STACK_MP) {
		MonoInst *ins;
		if (indirect)
			*indirect = TRUE;

		MONO_INST_NEW (cfg, ins, OP_LOADX_MEMBASE);
		ins->klass = klass;
		ins->sreg1 = src->dreg;
		ins->type = STACK_VTYPE;
		ins->dreg = alloc_ireg (cfg);
		MONO_ADD_INS (cfg->cbb, ins);
		return ins->dreg;
	}
	g_warning ("load_simd_vreg:: could not infer source simd (%d) vreg for op", src->type);
	mono_print_ins (src);
	g_assert_not_reached ();
}

static int
load_simd_vreg (MonoCompile *cfg, MonoMethod *cmethod, MonoInst *src, gboolean *indirect)
{
	return load_simd_vreg_class (cfg, cmethod->klass, src, indirect);
}

/* Create and emit a SIMD instruction, dreg is auto-allocated */
static MonoInst*
emit_simd_ins (MonoCompile *cfg, MonoClass *klass, int opcode, int sreg1, int sreg2)
{
	const char *spec = INS_INFO (opcode);
	MonoInst *ins;

	MONO_INST_NEW (cfg, ins, opcode);
	if (spec [MONO_INST_DEST] == 'x') {
		ins->dreg = alloc_xreg (cfg);
		ins->type = STACK_VTYPE;
	} else if (spec [MONO_INST_DEST] == 'i') {
		ins->dreg = alloc_ireg (cfg);
		ins->type = STACK_I4;
	} else if (spec [MONO_INST_DEST] == 'l') {
		ins->dreg = alloc_lreg (cfg);
		ins->type = STACK_I8;
	} else if (spec [MONO_INST_DEST] == 'f') {
		ins->dreg = alloc_freg (cfg);
		ins->type = STACK_R8;
	}
	ins->sreg1 = sreg1;
	ins->sreg2 = sreg2;
	ins->klass = klass;
	MONO_ADD_INS (cfg->cbb, ins);
	return ins;
}

static MonoInst*
emit_simd_ins_for_sig (MonoCompile *cfg, MonoClass *klass, int opcode, int instc0, int instc1, MonoMethodSignature *fsig, MonoInst **args)
{
	g_assert (fsig->param_count <= 3);
	MonoInst* ins = emit_simd_ins (cfg, klass, opcode,
		fsig->param_count > 0 ? args [0]->dreg : -1,
		fsig->param_count > 1 ? args [1]->dreg : -1);
	if (instc0 != -1)
		ins->inst_c0 = instc0;
	if (instc1 != -1)
		ins->inst_c1 = instc1;
	if (fsig->param_count == 3)
		ins->sreg3 = args [2]->dreg;
	return ins;
}

static gboolean
is_hw_intrinsics_class (MonoClass *klass, const char *name, gboolean *is_64bit)
{
	const char *class_name = m_class_get_name (klass);
	if ((!strcmp (class_name, "X64") || !strcmp (class_name, "Arm64")) && m_class_get_nested_in (klass)) {
		*is_64bit = TRUE;
		return !strcmp (m_class_get_name (m_class_get_nested_in (klass)), name);
	} else {
		*is_64bit = FALSE;
		return !strcmp (class_name, name);
	}
}

static MonoTypeEnum
get_underlying_type (MonoType* type)
{
	MonoClass* klass = mono_class_from_mono_type_internal (type);
	if (type->type == MONO_TYPE_PTR) // e.g. int* => MONO_TYPE_I4
		return m_class_get_byval_arg (m_class_get_element_class (klass))->type;
	else if (type->type == MONO_TYPE_GENERICINST) // e.g. Vector128<int> => MONO_TYPE_I4
		return mono_class_get_context (klass)->class_inst->type_argv [0]->type;
	else
		return type->type;
}

static MonoInst*
emit_xcompare (MonoCompile *cfg, MonoClass *klass, MonoTypeEnum etype, MonoInst *arg1, MonoInst *arg2)
{
	MonoInst *ins;
	gboolean is_fp = etype == MONO_TYPE_R4 || etype == MONO_TYPE_R8;

	ins = emit_simd_ins (cfg, klass, is_fp ? OP_XCOMPARE_FP : OP_XCOMPARE, arg1->dreg, arg2->dreg);
	ins->inst_c0 = CMP_EQ;
	ins->inst_c1 = etype;
	return ins;
}

static MonoType*
get_vector_t_elem_type (MonoType *vector_type)
{
	MonoClass *klass;
	MonoType *etype;

	g_assert (vector_type->type == MONO_TYPE_GENERICINST);
	klass = mono_class_from_mono_type_internal (vector_type);
	g_assert (
		!strcmp (m_class_get_name (klass), "Vector`1") || 
		!strcmp (m_class_get_name (klass), "Vector64`1") ||
		!strcmp (m_class_get_name (klass), "Vector128`1") || 
		!strcmp (m_class_get_name (klass), "Vector256`1"));
	etype = mono_class_get_context (klass)->class_inst->type_argv [0];
	return etype;
}

static gboolean
type_is_unsigned (MonoType *type) {
	MonoClass *klass = mono_class_from_mono_type_internal (type);
	MonoType *etype = mono_class_get_context (klass)->class_inst->type_argv [0];
	switch (etype->type) {
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		return TRUE;
	}
	return FALSE;
}

static int
type_to_expand_op (MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_EXPAND_I1;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_EXPAND_I2;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_EXPAND_I4;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_EXPAND_I8;
	case MONO_TYPE_R4:
		return OP_EXPAND_R4;
	case MONO_TYPE_R8:
		return OP_EXPAND_R8;
	default:
		g_assert_not_reached ();
	}
}

static int
type_to_insert_op (MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_INSERT_I1;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_INSERT_I2;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_INSERT_I4;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_INSERT_I8;
	case MONO_TYPE_R4:
		return OP_INSERT_R4;
	case MONO_TYPE_R8:
		return OP_INSERT_R8;
	default:
		g_assert_not_reached ();
	}
}

typedef struct {
	const char *name;
	MonoCPUFeatures feature;
	const SimdIntrinsic *intrinsics;
	int intrinsics_size;
	gboolean jit_supported;
} IntrinGroup;

typedef MonoInst * (* EmitIntrinsicFn) (
	MonoCompile *cfg, MonoMethodSignature *fsig, MonoInst **args,
	MonoClass *klass, const IntrinGroup *intrin_group,
	const SimdIntrinsic *info, int id, MonoTypeEnum arg0_type,
	gboolean is_64bit);

static MonoInst *
emit_hardware_intrinsics (
	MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig,
	MonoInst **args, const IntrinGroup *groups, int groups_size_bytes,
	EmitIntrinsicFn custom_emit)
{
	MonoClass *klass = cmethod->klass;
	const IntrinGroup *intrin_group = NULL;
	gboolean is_64bit = FALSE;
	int id = -1;
	int groups_size = groups_size_bytes / sizeof (groups [0]);
	for (int i = 0; i < groups_size; ++i) {
		const IntrinGroup *group = &groups [i];
		if (is_hw_intrinsics_class (klass, group->name, &is_64bit)) {
			intrin_group = group;
			break;
		}
	}

	const SimdIntrinsic *info = NULL;
	MonoInst *ins = NULL;
	gboolean supported = FALSE;
	MonoTypeEnum arg0_type = fsig->param_count > 0 ? get_underlying_type (fsig->params [0]) : MONO_TYPE_VOID;
	uint16_t op = 0;
	uint16_t c0 = 0;
	if (intrin_group) {
		const SimdIntrinsic *intrinsics = intrin_group->intrinsics;
		int intrinsics_size = intrin_group->intrinsics_size;
		MonoCPUFeatures feature = intrin_group->feature;

		// Hardware intrinsics are LLVM-only.
		if (!COMPILE_LLVM (cfg) && !intrin_group->jit_supported)
			goto support_probe_complete;

		info = lookup_intrins_info ((SimdIntrinsic *) intrinsics, intrinsics_size, cmethod);
		if (!info)
			goto support_probe_complete;

		if (feature)
			supported = ((mini_get_cpu_features (cfg) & feature) != 0) && (intrin_group->intrinsics != unsupported);
		else
			supported = TRUE;

		id = info->id;

		op = info->default_op;
		c0 = info->default_instc0;
		gboolean is_unsigned = FALSE;
		gboolean is_float = FALSE;
		switch (arg0_type) {
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
		case MONO_TYPE_U8:
			is_unsigned = TRUE;
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			is_float = TRUE;
			break;
		}
		if (is_unsigned && info->unsigned_op != 0) {
			op = info->unsigned_op;
			c0 = info->unsigned_instc0;
		} else if (is_float && info->floating_op != 0) {
			op = info->floating_op;
			c0 = info->floating_instc0;
		}
	}
support_probe_complete:
	if (id == SN_get_IsSupported) {
		EMIT_NEW_ICONST (cfg, ins, supported ? 1 : 0);
		return ins;
	}
	if (!supported) {
		// Can't emit non-supported llvm intrinsics
		if (!intrin_group || cfg->method != cmethod) {
			// Keep the original call so we end up in the intrinsic method
			return NULL;
		} else {
			// Emit an exception from the intrinsic method
			mono_emit_jit_icall (cfg, mono_throw_platform_not_supported, NULL);
			return NULL;
		}
	}
	if (op != 0)
		return emit_simd_ins_for_sig (cfg, klass, op, c0, arg0_type, fsig, args);
	return custom_emit (cfg, fsig, args, klass, intrin_group, info, id, arg0_type, is_64bit);
}

static MonoInst *
emit_vector_create_elementwise (
	MonoCompile *cfg, MonoMethodSignature *fsig, MonoType *vtype,
	MonoType *etype, MonoInst **args)
{
	int op = type_to_insert_op (etype);
	MonoClass *vklass = mono_class_from_mono_type_internal (vtype);
	MonoInst *ins = emit_simd_ins (cfg, vklass, OP_XZERO, -1, -1);
	for (int i = 0; i < fsig->param_count; ++i) {
		ins = emit_simd_ins (cfg, vklass, op, ins->dreg, args [i]->dreg);
		ins->inst_c0 = i;
	}
	return ins;
}

#if defined(TARGET_AMD64) || defined(TARGET_ARM64)

static guint16 sri_vector_methods [] = {
	SN_AsByte,
	SN_AsDouble,
	SN_AsInt16,
	SN_AsInt32,
	SN_AsInt64,
	SN_AsSByte,
	SN_AsSingle,
	SN_AsUInt16,
	SN_AsUInt32,
	SN_AsUInt64,
	SN_Create,
	SN_CreateScalarUnsafe,
};

static MonoInst*
emit_sri_vector (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	if (!COMPILE_LLVM (cfg))
		return NULL;

	MonoClass *klass = cmethod->klass;
	int id = lookup_intrins (sri_vector_methods, sizeof (sri_vector_methods), cmethod);
	if (id == -1)
		return NULL;

	if (!strcmp (m_class_get_name (cfg->method->klass), "Vector256"))
		return NULL; // TODO: Fix Vector256.WithUpper/WithLower

	MonoTypeEnum arg0_type = fsig->param_count > 0 ? get_underlying_type (fsig->params [0]) : MONO_TYPE_VOID;

	switch (id) {
	case SN_AsByte:
	case SN_AsDouble:
	case SN_AsInt16:
	case SN_AsInt32:
	case SN_AsInt64:
	case SN_AsSByte:
	case SN_AsSingle:
	case SN_AsUInt16:
	case SN_AsUInt32:
	case SN_AsUInt64:
		return emit_simd_ins (cfg, klass, OP_XCAST, args [0]->dreg, -1);
	case SN_Create: {
		MonoType *etype = get_vector_t_elem_type (fsig->ret);
		if (fsig->param_count == 1 && mono_metadata_type_equal (fsig->params [0], etype))
			return emit_simd_ins (cfg, klass, type_to_expand_op (etype), args [0]->dreg, -1);
		else
			return emit_vector_create_elementwise (cfg, fsig, fsig->ret, etype, args);
	}
	case SN_CreateScalarUnsafe:
		return emit_simd_ins_for_sig (cfg, klass, OP_CREATE_SCALAR_UNSAFE, -1, arg0_type, fsig, args);
	default:
		break;
	}

	return NULL;
}

#endif // defined(TARGET_AMD64) || defined(TARGET_ARM64)

#ifdef TARGET_AMD64

static guint16 vector_methods [] = {
	SN_ConvertToDouble,
	SN_ConvertToInt32,
	SN_ConvertToInt64,
	SN_ConvertToSingle,
	SN_ConvertToUInt32,
	SN_ConvertToUInt64,
	SN_Narrow,
	SN_Widen,
	SN_get_IsHardwareAccelerated,
};

static MonoInst*
emit_sys_numerics_vector (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *ins;
	gboolean supported = FALSE;
	int id;
	MonoType *etype;

	id = lookup_intrins (vector_methods, sizeof (vector_methods), cmethod);
	if (id == -1)
		return NULL;

	//printf ("%s\n", mono_method_full_name (cmethod, 1));

#ifdef MONO_ARCH_SIMD_INTRINSICS
	supported = TRUE;
#endif

	if (cfg->verbose_level > 1) {
		char *name = mono_method_full_name (cmethod, TRUE);
		printf ("  SIMD intrinsic %s\n", name);
		g_free (name);
	}

	switch (id) {
	case SN_get_IsHardwareAccelerated:
		EMIT_NEW_ICONST (cfg, ins, supported ? 1 : 0);
		ins->type = STACK_I4;
		return ins;
	case SN_ConvertToInt32:
		etype = get_vector_t_elem_type (fsig->params [0]);
		g_assert (etype->type == MONO_TYPE_R4);
		return emit_simd_ins (cfg, mono_class_from_mono_type_internal (fsig->ret), OP_CVTPS2DQ, args [0]->dreg, -1);
	case SN_ConvertToSingle:
		etype = get_vector_t_elem_type (fsig->params [0]);
		g_assert (etype->type == MONO_TYPE_I4 || etype->type == MONO_TYPE_U4);
		// FIXME:
		if (etype->type == MONO_TYPE_U4)
			return NULL;
		return emit_simd_ins (cfg, mono_class_from_mono_type_internal (fsig->ret), OP_CVTDQ2PS, args [0]->dreg, -1);
	case SN_ConvertToDouble:
	case SN_ConvertToInt64:
	case SN_ConvertToUInt32:
	case SN_ConvertToUInt64:
	case SN_Narrow:
	case SN_Widen:
		// FIXME:
		break;
	default:
		break;
	}

	return NULL;
}

static guint16 vector_t_methods [] = {
	SN_ctor,
	SN_CopyTo,
	SN_Equals,
	SN_GreaterThan,
	SN_GreaterThanOrEqual,
	SN_LessThan,
	SN_LessThanOrEqual,
	SN_Max,
	SN_Min,
	SN_get_AllBitsSet,
	SN_get_Count,
	SN_get_Item,
	SN_get_One,
	SN_get_Zero,
	SN_op_Addition,
	SN_op_BitwiseAnd,
	SN_op_BitwiseOr,
	SN_op_Division,
	SN_op_Equality,
	SN_op_ExclusiveOr,
	SN_op_Explicit,
	SN_op_Inequality,
	SN_op_Multiply,
	SN_op_Subtraction
};

static MonoInst*
emit_sys_numerics_vector_t (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *ins;
	MonoType *type, *etype;
	MonoClass *klass;
	int size, len, id;
	gboolean is_unsigned;

	static const float r4_one = 1.0f;
	static const double r8_one = 1.0;

	id = lookup_intrins (vector_t_methods, sizeof (vector_t_methods), cmethod);
	if (id == -1)
		return NULL;

	klass = cmethod->klass;
	type = m_class_get_byval_arg (klass);
	etype = mono_class_get_context (klass)->class_inst->type_argv [0];
	size = mono_class_value_size (mono_class_from_mono_type_internal (etype), NULL);
	g_assert (size);
	len = register_size / size;

	if (!MONO_TYPE_IS_PRIMITIVE (etype) || etype->type == MONO_TYPE_CHAR || etype->type == MONO_TYPE_BOOLEAN)
		return NULL;

	if (cfg->verbose_level > 1) {
		char *name = mono_method_full_name (cmethod, TRUE);
		printf ("  SIMD intrinsic %s\n", name);
		g_free (name);
	}

	switch (id) {
	case SN_get_Count:
		if (!(fsig->param_count == 0 && fsig->ret->type == MONO_TYPE_I4))
			break;
		EMIT_NEW_ICONST (cfg, ins, len);
		return ins;
	case SN_get_Zero:
		g_assert (fsig->param_count == 0 && mono_metadata_type_equal (fsig->ret, type));
		return emit_simd_ins (cfg, klass, OP_XZERO, -1, -1);
	case SN_get_One: {
		g_assert (fsig->param_count == 0 && mono_metadata_type_equal (fsig->ret, type));
		MonoInst *one = NULL;
		int expand_opcode = type_to_expand_op (etype);
		MONO_INST_NEW (cfg, one, -1);
		switch (expand_opcode) {
		case OP_EXPAND_R4:
			one->opcode = OP_R4CONST;
			one->type = STACK_R4;
			one->inst_p0 = (void *) &r4_one;
			break;
		case OP_EXPAND_R8:
			one->opcode = OP_R8CONST;
			one->type = STACK_R8;
			one->inst_p0 = (void *) &r8_one;
			break;
		default:
			one->opcode = OP_ICONST;
			one->type = STACK_I4;
			one->inst_c0 = 1;
			break;
		}
		one->dreg = alloc_dreg (cfg, (MonoStackType)one->type);
		MONO_ADD_INS (cfg->cbb, one);
		return emit_simd_ins (cfg, klass, expand_opcode, one->dreg, -1);
	}
	case SN_get_AllBitsSet: {
		/* Compare a zero vector with itself */
		ins = emit_simd_ins (cfg, klass, OP_XZERO, -1, -1);
		return emit_xcompare (cfg, klass, etype->type, ins, ins);
	}
	case SN_get_Item: {
		if (!COMPILE_LLVM (cfg))
			return NULL;
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, args [1]->dreg, len);
		MONO_EMIT_NEW_COND_EXC (cfg, GE_UN, "IndexOutOfRangeException");
		int opcode = -1;
		int dreg;
		gboolean is64 = FALSE;
		switch (etype->type) {
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			opcode = OP_XEXTRACT_I64;
			is64 = TRUE;
			dreg = alloc_lreg (cfg);
			break;
		case MONO_TYPE_R8:
			opcode = OP_XEXTRACT_R8;
			dreg = alloc_freg (cfg);
			break;
		case MONO_TYPE_R4:
			g_assert (cfg->r4fp);
			opcode = OP_XEXTRACT_R4;
			dreg = alloc_freg (cfg);
			break;
		default:
			opcode = OP_XEXTRACT_I32;
			dreg = alloc_ireg (cfg);
			break;
		}
		MONO_INST_NEW (cfg, ins, opcode);
		ins->dreg = dreg;
		ins->sreg1 = load_simd_vreg (cfg, cmethod, args [0], NULL);
		ins->sreg2 = args [1]->dreg;
		ins->inst_c0 = etype->type;
		mini_type_to_eval_stack_type (cfg, etype, ins);
		MONO_ADD_INS (cfg->cbb, ins);
		return ins;
	}
	case SN_ctor:
		if (fsig->param_count == 1 && mono_metadata_type_equal (fsig->params [0], etype)) {
			int dreg = load_simd_vreg (cfg, cmethod, args [0], NULL);

			int opcode = type_to_expand_op (etype);
			ins = emit_simd_ins (cfg, klass, opcode, args [1]->dreg, -1);
			ins->dreg = dreg;
			return ins;
		}
		if ((fsig->param_count == 1 || fsig->param_count == 2) && (fsig->params [0]->type == MONO_TYPE_SZARRAY)) {
			MonoInst *array_ins = args [1];
			MonoInst *index_ins;
			MonoInst *ldelema_ins;
			MonoInst *var;
			int end_index_reg;

			if (args [0]->opcode != OP_LDADDR)
				return NULL;

			/* .ctor (T[]) or .ctor (T[], index) */

			if (fsig->param_count == 2) {
				index_ins = args [2];
			} else {
				EMIT_NEW_ICONST (cfg, index_ins, 0);
			}

			/* Emit index check for the end (index + len - 1 < array length) */
			end_index_reg = alloc_ireg (cfg);
			EMIT_NEW_BIALU_IMM (cfg, ins, OP_IADD_IMM, end_index_reg, index_ins->dreg, len - 1);
			MONO_EMIT_BOUNDS_CHECK (cfg, array_ins->dreg, MonoArray, max_length, end_index_reg);

			/* Load the array slice into the simd reg */
			ldelema_ins = mini_emit_ldelema_1_ins (cfg, mono_class_from_mono_type_internal (etype), array_ins, index_ins, TRUE, FALSE);
			g_assert (args [0]->opcode == OP_LDADDR);
			var = (MonoInst*)args [0]->inst_p0;
			EMIT_NEW_LOAD_MEMBASE (cfg, ins, OP_LOADX_MEMBASE, var->dreg, ldelema_ins->dreg, 0);
			ins->klass = cmethod->klass;
			return args [0];
		}
		break;
	case SN_CopyTo:
		if ((fsig->param_count == 1 || fsig->param_count == 2) && (fsig->params [0]->type == MONO_TYPE_SZARRAY)) {
			MonoInst *array_ins = args [1];
			MonoInst *index_ins;
			MonoInst *ldelema_ins;
			int val_vreg, end_index_reg;

			val_vreg = load_simd_vreg (cfg, cmethod, args [0], NULL);

			/* CopyTo (T[]) or CopyTo (T[], index) */

			if (fsig->param_count == 2) {
				index_ins = args [2];
			} else {
				EMIT_NEW_ICONST (cfg, index_ins, 0);
			}

			/* CopyTo () does complicated argument checks */
			mini_emit_bounds_check_offset (cfg, array_ins->dreg, MONO_STRUCT_OFFSET (MonoArray, max_length), index_ins->dreg, "ArgumentOutOfRangeException");
			end_index_reg = alloc_ireg (cfg);
			int len_reg = alloc_ireg (cfg);
			MONO_EMIT_NEW_LOAD_MEMBASE_OP_FLAGS (cfg, OP_LOADI4_MEMBASE, len_reg, array_ins->dreg, MONO_STRUCT_OFFSET (MonoArray, max_length), MONO_INST_INVARIANT_LOAD);
			EMIT_NEW_BIALU (cfg, ins, OP_ISUB, end_index_reg, len_reg, index_ins->dreg);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, end_index_reg, len);
			MONO_EMIT_NEW_COND_EXC (cfg, LT, "ArgumentException");

			/* Load the array slice into the simd reg */
			ldelema_ins = mini_emit_ldelema_1_ins (cfg, mono_class_from_mono_type_internal (etype), array_ins, index_ins, FALSE, FALSE);
			EMIT_NEW_STORE_MEMBASE (cfg, ins, OP_STOREX_MEMBASE, ldelema_ins->dreg, 0, val_vreg);
			ins->klass = cmethod->klass;
			return ins;
		}
		break;
	case SN_Equals:
		if (fsig->param_count == 1 && fsig->ret->type == MONO_TYPE_BOOLEAN && mono_metadata_type_equal (fsig->params [0], type)) {
			int sreg1 = load_simd_vreg (cfg, cmethod, args [0], NULL);

			return emit_simd_ins (cfg, klass, OP_XEQUAL, sreg1, args [1]->dreg);
		} else if (fsig->param_count == 2 && mono_metadata_type_equal (fsig->ret, type) && mono_metadata_type_equal (fsig->params [0], type) && mono_metadata_type_equal (fsig->params [1], type)) {
			/* Per element equality */
			return emit_xcompare (cfg, klass, etype->type, args [0], args [1]);
		}
		break;
	case SN_op_Equality:
	case SN_op_Inequality:
		g_assert (fsig->param_count == 2 && fsig->ret->type == MONO_TYPE_BOOLEAN &&
				  mono_metadata_type_equal (fsig->params [0], type) &&
				  mono_metadata_type_equal (fsig->params [1], type));
		ins = emit_simd_ins (cfg, klass, OP_XEQUAL, args [0]->dreg, args [1]->dreg);
		if (id == SN_op_Inequality) {
			int sreg = ins->dreg;
			int dreg = alloc_ireg (cfg);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, sreg, 0);
			EMIT_NEW_UNALU (cfg, ins, OP_CEQ, dreg, -1);
		}
		return ins;
	case SN_GreaterThan:
	case SN_GreaterThanOrEqual:
	case SN_LessThan:
	case SN_LessThanOrEqual:
		g_assert (fsig->param_count == 2 && mono_metadata_type_equal (fsig->ret, type) && mono_metadata_type_equal (fsig->params [0], type) && mono_metadata_type_equal (fsig->params [1], type));
		is_unsigned = etype->type == MONO_TYPE_U1 || etype->type == MONO_TYPE_U2 || etype->type == MONO_TYPE_U4 || etype->type == MONO_TYPE_U8;
		ins = emit_xcompare (cfg, klass, etype->type, args [0], args [1]);
		switch (id) {
		case SN_GreaterThan:
			ins->inst_c0 = is_unsigned ? CMP_GT_UN : CMP_GT;
			break;
		case SN_GreaterThanOrEqual:
			ins->inst_c0 = is_unsigned ? CMP_GE_UN : CMP_GE;
			break;
		case SN_LessThan:
			ins->inst_c0 = is_unsigned ? CMP_LT_UN : CMP_LT;
			break;
		case SN_LessThanOrEqual:
			ins->inst_c0 = is_unsigned ? CMP_LE_UN : CMP_LE;
			break;
		default:
			g_assert_not_reached ();
		}
		return ins;
	case SN_op_Explicit:
		return emit_simd_ins (cfg, klass, OP_XCAST, args [0]->dreg, -1);
	case SN_op_Addition:
	case SN_op_Subtraction:
	case SN_op_Division:
	case SN_op_Multiply:
	case SN_op_BitwiseAnd:
	case SN_op_BitwiseOr:
	case SN_op_ExclusiveOr:
	case SN_Max:
	case SN_Min:
		if (!(fsig->param_count == 2 && mono_metadata_type_equal (fsig->ret, type) && mono_metadata_type_equal (fsig->params [0], type) && mono_metadata_type_equal (fsig->params [1], type)))
			return NULL;
		ins = emit_simd_ins (cfg, klass, OP_XBINOP, args [0]->dreg, args [1]->dreg);
		ins->inst_c1 = etype->type;

		if (etype->type == MONO_TYPE_R4 || etype->type == MONO_TYPE_R8) {
			switch (id) {
			case SN_op_Addition:
				ins->inst_c0 = OP_FADD;
				break;
			case SN_op_Subtraction:
				ins->inst_c0 = OP_FSUB;
				break;
			case SN_op_Multiply:
				ins->inst_c0 = OP_FMUL;
				break;
			case SN_op_Division:
				ins->inst_c0 = OP_FDIV;
				break;
			case SN_Max:
				ins->inst_c0 = OP_FMAX;
				break;
			case SN_Min:
				ins->inst_c0 = OP_FMIN;
				break;
			default:
				NULLIFY_INS (ins);
				return NULL;
			}
		} else {
			switch (id) {
			case SN_op_Addition:
				ins->inst_c0 = OP_IADD;
				break;
			case SN_op_Subtraction:
				ins->inst_c0 = OP_ISUB;
				break;
				/*
			case SN_op_Division:
				ins->inst_c0 = OP_IDIV;
				break;
			case SN_op_Multiply:
				ins->inst_c0 = OP_IMUL;
				break;
				*/
			case SN_op_BitwiseAnd:
				ins->inst_c0 = OP_IAND;
				break;
			case SN_op_BitwiseOr:
				ins->inst_c0 = OP_IOR;
				break;
			case SN_op_ExclusiveOr:
				ins->inst_c0 = OP_IXOR;
				break;
			case SN_Max:
				ins->inst_c0 = OP_IMAX;
				break;
			case SN_Min:
				ins->inst_c0 = OP_IMIN;
				break;
			default:
				NULLIFY_INS (ins);
				return NULL;
			}
		}
		return ins;
	default:
		break;
	}

	return NULL;
}
#endif // TARGET_AMD64

static MonoInst*
emit_invalid_operation (MonoCompile *cfg, const char* message)
{
	mono_cfg_set_exception (cfg, MONO_EXCEPTION_MONO_ERROR);
	mono_error_set_generic_error (cfg->error, "System", "InvalidOperationException", "%s", message);
	return NULL;
}

#ifdef TARGET_ARM64


static SimdIntrinsic armbase_methods [] = {
	{SN_LeadingSignCount},
	{SN_LeadingZeroCount},
	{SN_MultiplyHigh},
	{SN_ReverseElementBits},
	{SN_get_IsSupported},
};

static SimdIntrinsic crc32_methods [] = {
	{SN_ComputeCrc32},
	{SN_ComputeCrc32C},
	{SN_get_IsSupported}
};

static SimdIntrinsic crypto_aes_methods [] = {
	{SN_Decrypt, OP_XOP_X_X_X, SIMD_OP_AES_DEC},
	{SN_Encrypt, OP_XOP_X_X_X, SIMD_OP_AES_ENC},
	{SN_InverseMixColumns, OP_XOP_X_X, SIMD_OP_AES_IMC},
	{SN_MixColumns, OP_XOP_X_X, SIMD_OP_ARM64_AES_AESMC},
	{SN_PolynomialMultiplyWideningLower, OP_XOP_X_X_X, SIMD_OP_ARM64_PMULL64_LOWER},
	{SN_PolynomialMultiplyWideningUpper, OP_XOP_X_X_X, SIMD_OP_ARM64_PMULL64_UPPER},
	{SN_get_IsSupported},
};

static SimdIntrinsic sha1_methods [] = {
	{SN_FixedRotate, OP_XOP_X_X, SIMD_OP_ARM64_SHA1H},
	{SN_HashUpdateChoose, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA1C},
	{SN_HashUpdateMajority, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA1M},
	{SN_HashUpdateParity, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA1P},
	{SN_ScheduleUpdate0, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA1SU0},
	{SN_ScheduleUpdate1, OP_XOP_X_X_X, SIMD_OP_ARM64_SHA1SU1},
	{SN_get_IsSupported}
};

static SimdIntrinsic sha256_methods [] = {
	{SN_HashUpdate1, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA256H},
	{SN_HashUpdate2, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA256H2},
	{SN_ScheduleUpdate0, OP_XOP_X_X_X, SIMD_OP_ARM64_SHA256SU0},
	{SN_ScheduleUpdate1, OP_XOP_X_X_X_X, SIMD_OP_ARM64_SHA256SU1},
	{SN_get_IsSupported}
};

// This table must be kept in sorted order. ASCII } is sorted after alphanumeric
// characters, so blind use of your editor's "sort lines" facility will
// mis-order the lines.
//
// In Vim you can use `sort /.*{[0-9A-z]*/ r` to sort this table.

static SimdIntrinsic advsimd_methods [] = {
	{SN_Abs},
	{SN_AbsSaturate},
	{SN_AbsScalar},
	{SN_AbsoluteCompareGreaterThan},
	{SN_AbsoluteCompareGreaterThanOrEqual},
	{SN_AbsoluteCompareLessThan},
	{SN_AbsoluteCompareLessThanOrEqual},
	{SN_MultiplyWideningUpperAndSubtract, OP_ARM64_SMLSL2},
	{SN_Negate, OP_ARM64_XNEG},
	{SN_NegateSaturate, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_SQNEG},
	{SN_NegateSaturateScalar},
	{SN_NegateScalar, OP_ARM64_XNEG_SCALAR},
	{SN_Not, OP_XBINOP_FORCEINT, XBINOP_FORCEINT_Not},
	{SN_Or, OP_XBINOP_FORCEINT, XBINOP_FORCEINT_Or},
	{SN_OrNot, OP_XBINOP_FORCEINT, XBINOP_FORCEINT_OrNot},
	{SN_PolynomialMultiply, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_PMUL},
	{SN_PolynomialMultiplyWideningLower, OP_ARM64_PMULL},
	{SN_PolynomialMultiplyWideningUpper, OP_ARM64_PMULL2},
	{SN_PopCount, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_CNT},
	{SN_ReciprocalEstimate},
	{SN_ReciprocalEstimateScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRECPE},
	{SN_ReciprocalExponentScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRECPX},
	{SN_ReciprocalSquareRootEstimate},
	{SN_ReciprocalSquareRootEstimateScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRSQRTE},
	{SN_ReciprocalStep, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRECPS},
	{SN_ReciprocalStepScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRECPS},
	{SN_ReverseElement16, OP_ARM64_REV32},
	{SN_ReverseElement32, OP_ARM64_REV64},
	{SN_ReverseElement8, OP_ARM64_REV16},
	{SN_ReverseElementBits, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_RBIT},
	{SN_RoundAwayFromZero, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTA},
	{SN_RoundAwayFromZeroScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTA},
	{SN_RoundToNearest, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTN},
	{SN_RoundToNearestScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTN},
	{SN_RoundToNegativeInfinity, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTM},
	{SN_RoundToNegativeInfinityScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTM},
	{SN_RoundToPositiveInfinity, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTP},
	{SN_RoundToPositiveInfinityScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTP},
	{SN_RoundToZero, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTZ},
	{SN_RoundToZeroScalar, OP_XOP_OVR_SCALAR_X_X, INTRINS_AARCH64_ADV_SIMD_FRINTZ},
	{SN_ShiftArithmetic, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SSHL},
	{SN_ShiftArithmeticRounded, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SRSHL},
	{SN_ShiftArithmeticRoundedSaturate, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQRSHL},
	{SN_ShiftArithmeticRoundedSaturateScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQRSHL},
	{SN_ShiftArithmeticRoundedScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SRSHL},
	{SN_ShiftArithmeticSaturate, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQSHL},
	{SN_ShiftArithmeticSaturateScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQSHL},
	{SN_ShiftArithmeticScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SSHL},
	{SN_ShiftLeftAndInsert, OP_ARM64_SRI},
	{SN_ShiftLeftAndInsertScalar, OP_ARM64_SRI},
	{SN_ShiftLeftLogical, OP_ARM64_SHL},
	{SN_ShiftLeftLogicalSaturate},
	{SN_ShiftLeftLogicalSaturateScalar},
	{SN_ShiftLeftLogicalSaturateUnsigned, OP_ARM64_SQSHLU},
	{SN_ShiftLeftLogicalSaturateUnsignedScalar, OP_ARM64_SQSHLU},
	{SN_ShiftLeftLogicalScalar, OP_ARM64_SHL},
	{SN_ShiftLeftLogicalWideningLower, OP_ARM64_USHLL},
	{SN_ShiftLeftLogicalWideningUpper, OP_ARM64_USHLL2},
	{SN_ShiftLogical, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_USHL},
	{SN_ShiftLogicalRounded, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_URSHL},
	{SN_ShiftLogicalRoundedSaturate, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQRSHL},
	{SN_ShiftLogicalRoundedSaturateScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQRSHL},
	{SN_ShiftLogicalRoundedScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_URSHL},
	{SN_ShiftLogicalSaturate, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQSHL},
	{SN_ShiftLogicalSaturateScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQSHL},
	{SN_ShiftLogicalScalar, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_USHL},
	{SN_ShiftRightAndInsert, OP_ARM64_SRI},
	{SN_ShiftRightAndInsertScalar, OP_ARM64_SRI},
	{SN_ShiftRightArithmetic, OP_ARM64_SSHR},
	{SN_ShiftRightArithmeticAdd, OP_ARM64_SSRA},
	{SN_ShiftRightArithmeticAddScalar, OP_ARM64_SSRA},
	{SN_ShiftRightArithmeticNarrowingSaturateLower, OP_ARM64_SQSHRN},
	{SN_ShiftRightArithmeticNarrowingSaturateUnsignedLower, OP_ARM64_SQSHRUN},
	{SN_ShiftRightArithmeticNarrowingSaturateUnsignedUpper, OP_ARM64_SQSHRUN2},
	{SN_ShiftRightArithmeticNarrowingSaturateUpper, OP_ARM64_SQSHRN2},
	{SN_ShiftRightArithmeticRounded, OP_ARM64_SRSHR},
	{SN_ShiftRightArithmeticRoundedAdd, OP_ARM64_SRSRA},
	{SN_ShiftRightArithmeticRoundedAddScalar, OP_ARM64_SRSRA},
	{SN_ShiftRightArithmeticRoundedNarrowingSaturateLower, OP_ARM64_SQRSHRN},
	{SN_ShiftRightArithmeticRoundedNarrowingSaturateUnsignedLower, OP_ARM64_SQRSHRUN},
	{SN_ShiftRightArithmeticRoundedNarrowingSaturateUnsignedUpper, OP_ARM64_SQRSHRUN2},
	{SN_ShiftRightArithmeticRoundedNarrowingSaturateUpper, OP_ARM64_SQRSHRN2},
	{SN_ShiftRightArithmeticRoundedScalar, OP_ARM64_SRSHR},
	{SN_ShiftRightArithmeticScalar, OP_ARM64_SSHR},
	{SN_ShiftRightLogical, OP_ARM64_USHR},
	{SN_ShiftRightLogicalAdd, OP_ARM64_USRA},
	{SN_ShiftRightLogicalAddScalar, OP_ARM64_USRA},
	{SN_ShiftRightLogicalNarrowingLower, OP_ARM64_SHRN},
	{SN_ShiftRightLogicalNarrowingSaturateLower, OP_ARM64_UQSHRN},
	{SN_ShiftRightLogicalNarrowingSaturateUpper, OP_ARM64_UQSHRN2},
	{SN_ShiftRightLogicalNarrowingUpper, OP_ARM64_SHRN2},
	{SN_ShiftRightLogicalRounded, OP_ARM64_URSHR},
	{SN_ShiftRightLogicalRoundedAdd, OP_ARM64_URSRA},
	{SN_ShiftRightLogicalRoundedAddScalar, OP_ARM64_URSRA},
	{SN_ShiftRightLogicalRoundedNarrowingLower, OP_ARM64_RSHRN},
	{SN_ShiftRightLogicalRoundedNarrowingSaturateLower, OP_ARM64_UQRSHRN},
	{SN_ShiftRightLogicalRoundedNarrowingSaturateScalar},
	{SN_ShiftRightLogicalRoundedNarrowingSaturateUpper, OP_ARM64_UQRSHRN2},
	{SN_ShiftRightLogicalRoundedNarrowingUpper, OP_ARM64_RSHRN2},
	{SN_ShiftRightLogicalRoundedScalar, OP_ARM64_URSHR},
	{SN_ShiftRightLogicalScalar, OP_ARM64_USHR},
	{SN_SignExtendWideningLower, OP_ARM64_SXTL},
	{SN_SignExtendWideningUpper, OP_ARM64_SXTL2},
	{SN_Sqrt, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_FSQRT},
	{SN_SqrtScalar, OP_ARM64_SQRT_SCALAR},
	{SN_Store, OP_ARM64_ST1},
	{SN_StorePair, OP_ARM64_STP},
	{SN_StorePairNonTemporal, OP_ARM64_STNP},
	{SN_StorePairScalar, OP_ARM64_STP_SCALAR},
	{SN_StorePairScalarNonTemporal, OP_ARM64_STNP_SCALAR},
	{SN_StoreSelectedScalar, OP_ARM64_ST1_SCALAR},
	{SN_Subtract, OP_XBINOP, OP_ISUB, None, None, OP_XBINOP_SCALAR, OP_FSUB},
	{SN_SubtractHighNarrowingLower, OP_ARM64_SUBHN},
	{SN_SubtractHighNarrowingUpper, OP_ARM64_SUBHN2},
	{SN_SubtractRoundedHighNarrowingLower, OP_ARM64_RSUBHN},
	{SN_SubtractRoundedHighNarrowingUpper, OP_ARM64_RSUBHN2},
	{SN_SubtractSaturate, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQSUB, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQSUB},
	{SN_SubtractSaturateScalar, OP_XOP_OVR_SCALAR_X_X_X, INTRINS_AARCH64_ADV_SIMD_SQSUB, OP_XOP_OVR_SCALAR_X_X_X, INTRINS_AARCH64_ADV_SIMD_UQSUB},
	{SN_SubtractScalar, OP_XBINOP_SCALAR, OP_ISUB, None, None, OP_XBINOP_SCALAR, OP_FSUB},
	{SN_SubtractWideningLower, OP_ARM64_SSUB, None, OP_ARM64_USUB},
	{SN_SubtractWideningUpper, OP_ARM64_SSUB2, None, OP_ARM64_USUB2},
	{SN_TransposeEven, OP_ARM64_TRN1},
	{SN_TransposeOdd, OP_ARM64_TRN2},
	{SN_UnzipEven, OP_ARM64_UZP1},
	{SN_UnzipOdd, OP_ARM64_UZP2},
	{SN_VectorTableLookup, OP_XOP_OVR_X_X_X, INTRINS_AARCH64_ADV_SIMD_TBL1},
	{SN_VectorTableLookupExtension, OP_XOP_OVR_X_X_X_X, INTRINS_AARCH64_ADV_SIMD_TBX1},
	{SN_Xor, OP_XBINOP_FORCEINT, XBINOP_FORCEINT_Xor},
	{SN_ZeroExtendWideningLower, OP_ARM64_UXTL},
	{SN_ZeroExtendWideningUpper, OP_ARM64_UXTL2},
	{SN_ZipHigh, OP_ARM64_ZIP2},
	{SN_ZipLow, OP_ARM64_ZIP1},
	{SN_get_IsSupported},
};

static
MonoInst *emit_absolute_compare (MonoCompile *cfg, MonoClass *klass, MonoMethodSignature *fsig, MonoTypeEnum arg0_type, MonoInst **args, SimdOp op_for_r4, SimdOp op_for_r8)
{
	SimdOp op = (SimdOp)0;

	switch (get_underlying_type (fsig->params [0])) {
	case MONO_TYPE_R4:
		op = op_for_r4;
	  	break;
	case MONO_TYPE_R8:
		op = op_for_r8;
		break;
	default:
		g_assert_not_reached();
	}

	return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, op, arg0_type, fsig, args);
}

static const IntrinGroup supported_arm_intrinsics [] = {
	{ "AdvSimd", MONO_CPU_ARM64_NEON, advsimd_methods, sizeof (advsimd_methods) },
	{ "Aes", MONO_CPU_ARM64_CRYPTO, crypto_aes_methods, sizeof (crypto_aes_methods) },
	{ "ArmBase", MONO_CPU_ARM64_BASE, armbase_methods, sizeof (armbase_methods) },
	{ "Crc32", MONO_CPU_ARM64_CRC, crc32_methods, sizeof (crc32_methods) },
	{ "Dp", MONO_CPU_ARM64_DP, unsupported, sizeof (unsupported) },
	{ "Rdm", MONO_CPU_ARM64_RDM, unsupported, sizeof (unsupported) },
	{ "Sha1", MONO_CPU_ARM64_CRYPTO, sha1_methods, sizeof (sha1_methods) },
	{ "Sha256", MONO_CPU_ARM64_CRYPTO, sha256_methods, sizeof (sha256_methods) },
};

static MonoInst*
emit_arm64_intrinsics (
	MonoCompile *cfg, MonoMethodSignature *fsig, MonoInst **args,
	MonoClass *klass, const IntrinGroup *intrin_group,
	const SimdIntrinsic *info, int id, MonoTypeEnum arg0_type,
	gboolean is_64bit)
{
	MonoCPUFeatures feature = intrin_group->feature;

	gboolean arg0_i32 = (arg0_type == MONO_TYPE_I4) || (arg0_type == MONO_TYPE_U4);

	if (feature == MONO_CPU_ARM64_BASE) {
		switch (id) {
		case SN_LeadingZeroCount:
			return emit_simd_ins_for_sig (cfg, klass, arg0_i32 ? OP_LZCNT32 : OP_LZCNT64, 0, arg0_type, fsig, args);
		case SN_LeadingSignCount:
			return emit_simd_ins_for_sig (cfg, klass, arg0_i32 ? OP_LSCNT32 : OP_LSCNT64, 0, arg0_type, fsig, args);
		case SN_MultiplyHigh:
			return emit_simd_ins_for_sig (cfg, klass,
				(arg0_type == MONO_TYPE_I8 ? OP_ARM64_SMULH : OP_ARM64_UMULH), 0, arg0_type, fsig, args);
		case SN_ReverseElementBits:
			return emit_simd_ins_for_sig (cfg, klass,
				(is_64bit ? OP_XOP_I8_I8 : OP_XOP_I4_I4),
				(is_64bit ? SIMD_OP_ARM64_RBIT64 : SIMD_OP_ARM64_RBIT32),
				arg0_type, fsig, args);
		default:
			g_assert_not_reached (); // if a new API is added we need to either implement it or change IsSupported to false
		}
	}

	if (feature == MONO_CPU_ARM64_CRC) {
		switch (id) {
		case SN_ComputeCrc32:
		case SN_ComputeCrc32C: {
			SimdOp op = (SimdOp)0;
			gboolean is_c = info->id == SN_ComputeCrc32C;
			switch (get_underlying_type (fsig->params [1])) {
			case MONO_TYPE_U1: op = is_c ? SIMD_OP_ARM64_CRC32CB : SIMD_OP_ARM64_CRC32B; break;
			case MONO_TYPE_U2: op = is_c ? SIMD_OP_ARM64_CRC32CH : SIMD_OP_ARM64_CRC32H; break;
			case MONO_TYPE_U4: op = is_c ? SIMD_OP_ARM64_CRC32CW : SIMD_OP_ARM64_CRC32W; break;
			case MONO_TYPE_U8: op = is_c ? SIMD_OP_ARM64_CRC32CX : SIMD_OP_ARM64_CRC32X; break;
			default: g_assert_not_reached (); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, is_64bit ? OP_XOP_I4_I4_I8 : OP_XOP_I4_I4_I4, op, arg0_type, fsig, args);
		}
		default:
			g_assert_not_reached (); // if a new API is added we need to either implement it or change IsSupported to false
		}
	}

	if (feature == MONO_CPU_ARM64_NEON) {
		SimdOp op = (SimdOp) 0;
		IntrinsicId iid = (IntrinsicId) -1;
		switch (id) {
		case SN_Abs: {
			// HACK: Temporary, while Vector64 support is completed
			MonoClass *arg0_klass = mono_class_from_mono_type_internal (fsig->params [0]);
			if (m_class_get_name (arg0_klass), "Vector64`1")
				mono_emit_jit_icall (cfg, mono_throw_platform_not_supported, NULL);

			switch (arg0_type) {
			case MONO_TYPE_R8:
				op = SIMD_OP_ARM64_DABS;
				break;
			case MONO_TYPE_R4:
				op = SIMD_OP_ARM64_FABS;
				break;
			case MONO_TYPE_I1:
				op = SIMD_OP_ARM64_I8ABS;
				break;
			case MONO_TYPE_I2:
				op = SIMD_OP_ARM64_I16ABS;
				break;
			case MONO_TYPE_I4:
				op = SIMD_OP_ARM64_I32ABS;
				break;
			case MONO_TYPE_I8:
				op = SIMD_OP_ARM64_I64ABS;
				break;
			}
		}

		case SN_AbsoluteCompareGreaterThan: {
			return emit_absolute_compare (cfg, klass, fsig, arg0_type, args, SIMD_OP_ARM64_FABSOLUTE_COMPARE_GREATER_THAN, SIMD_OP_ARM64_DABSOLUTE_COMPARE_GREATER_THAN);
		}

	    	case SN_AbsoluteCompareGreaterThanOrEqual: {
			return emit_absolute_compare (cfg, klass, fsig, arg0_type, args, SIMD_OP_ARM64_FABSOLUTE_COMPARE_GREATER_THAN_OR_EQUAL, SIMD_OP_ARM64_DABSOLUTE_COMPARE_GREATER_THAN_OR_EQUAL);
		}

		case SN_AbsoluteCompareLessThan: {
			// Compare less than uses the same instructions as greater than, with arguments swapped.
			MonoInst *temp_for_swap = args [0];
			args [0] = args [1];
			args [1] = temp_for_swap;

			return emit_absolute_compare (cfg, klass, fsig, arg0_type, args, SIMD_OP_ARM64_FABSOLUTE_COMPARE_LESS_THAN, SIMD_OP_ARM64_DABSOLUTE_COMPARE_LESS_THAN);
		}

		case SN_AbsoluteCompareLessThanOrEqual: {
			// Compare less than uses the same instructions as greater than, with arguments swapped.
			MonoInst *temp_for_swap = args [0];
			args [0] = args [1];
			args [1] = temp_for_swap;

			return emit_absolute_compare (cfg, klass, fsig, arg0_type, args, SIMD_OP_ARM64_FABSOLUTE_COMPARE_LESS_THAN_OR_EQUAL, SIMD_OP_ARM64_DABSOLUTE_COMPARE_LESS_THAN_OR_EQUAL);
		}

		case SN_AbsSaturate: {
			switch (arg0_type) {
			case MONO_TYPE_I1: op = SIMD_OP_ARM64_I8ABS_SATURATE; break;
			case MONO_TYPE_I2: op = SIMD_OP_ARM64_I16ABS_SATURATE; break;
			case MONO_TYPE_I4: op = SIMD_OP_ARM64_I32ABS_SATURATE; break;
			case MONO_TYPE_I8: op = SIMD_OP_ARM64_I64ABS_SATURATE; break;
			default: g_assert_not_reached ();
			}

			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X, op, arg0_type, fsig, args);
		}

		case SN_AbsScalar: {
			switch (arg0_type) {
			case MONO_TYPE_I1: op = SIMD_OP_ARM64_I8ABS_SATURATE; break;
			case MONO_TYPE_I2: op = SIMD_OP_ARM64_I16ABS_SATURATE; break;
			case MONO_TYPE_I4: op = SIMD_OP_ARM64_I32ABS_SATURATE; break;
			case MONO_TYPE_I8: op = SIMD_OP_ARM64_I64ABS_SATURATE; break;
			default: g_assert_not_reached ();
			}
			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X, op, arg0_type, fsig, args);
		}
		case SN_NegateSaturateScalar: {
			MonoInst *ret = emit_simd_ins_for_sig (cfg, klass, OP_XOP_OVR_X_X, INTRINS_AARCH64_ADV_SIMD_SQNEG, arg0_type, fsig, args);
			ret = emit_simd_ins (cfg, klass, OP_ARM64_ZERO_UPPER, ret->dreg, -1);
			return ret;
		}
		case SN_ReciprocalEstimate:
		case SN_ReciprocalSquareRootEstimate: {
			gboolean is_float = FALSE;
			switch (arg0_type) {
			case MONO_TYPE_R4: case MONO_TYPE_R8: is_float = TRUE;
			}
			switch (id) {
			case SN_ReciprocalEstimate:
				iid = is_float ? INTRINS_AARCH64_ADV_SIMD_FRECPE: INTRINS_AARCH64_ADV_SIMD_URECPE;
				break;
			case SN_ReciprocalSquareRootEstimate:
				iid = is_float ? INTRINS_AARCH64_ADV_SIMD_FRSQRTE : INTRINS_AARCH64_ADV_SIMD_URSQRTE;
				break;
			}
			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_OVR_X_X, iid, arg0_type, fsig, args);
		}
		case SN_ShiftLeftLogicalSaturate:
		case SN_ShiftLeftLogicalSaturateScalar: {
			gboolean is_unsigned = type_is_unsigned (fsig->ret);
			op = is_unsigned ? OP_ARM64_UQSHL_IMM : OP_ARM64_SQSHL_IMM;
			return emit_simd_ins_for_sig (cfg, klass, op, 0, arg0_type, fsig, args);
		}
		case SN_ShiftRightLogicalRoundedNarrowingSaturateScalar: {
			MonoInst *ret = emit_simd_ins_for_sig (cfg, klass, OP_ARM64_UQRSHRN, 0, arg0_type, fsig, args);
			ret = emit_simd_ins (cfg, klass, OP_ARM64_ZERO_UPPER, ret->dreg, -1);
			return ret;
		}
		default:
			g_assert_not_reached ();
		}
	}

	return NULL;
}
#endif // TARGET_ARM64

#ifdef TARGET_AMD64

static SimdIntrinsic sse_methods [] = {
	{SN_Add, OP_XBINOP, OP_FADD},
	{SN_AddScalar, OP_SSE_ADDSS},
	{SN_And, OP_SSE_AND},
	{SN_AndNot, OP_SSE_ANDN},
	{SN_CompareEqual, OP_XCOMPARE_FP, CMP_EQ},
	{SN_CompareGreaterThan, OP_XCOMPARE_FP,CMP_GT},
	{SN_CompareGreaterThanOrEqual, OP_XCOMPARE_FP, CMP_GE},
	{SN_CompareLessThan, OP_XCOMPARE_FP, CMP_LT},
	{SN_CompareLessThanOrEqual, OP_XCOMPARE_FP, CMP_LE},
	{SN_CompareNotEqual, OP_XCOMPARE_FP, CMP_NE},
	{SN_CompareNotGreaterThan, OP_XCOMPARE_FP, CMP_LE},
	{SN_CompareNotGreaterThanOrEqual, OP_XCOMPARE_FP, CMP_LT},
	{SN_CompareNotLessThan, OP_XCOMPARE_FP, CMP_GE},
	{SN_CompareNotLessThanOrEqual, OP_XCOMPARE_FP, CMP_GT},
	{SN_CompareOrdered, OP_XCOMPARE_FP, CMP_ORD},
	{SN_CompareScalarEqual, OP_SSE_CMPSS, CMP_EQ},
	{SN_CompareScalarGreaterThan, OP_SSE_CMPSS, CMP_GT},
	{SN_CompareScalarGreaterThanOrEqual, OP_SSE_CMPSS, CMP_GE},
	{SN_CompareScalarLessThan, OP_SSE_CMPSS, CMP_LT},
	{SN_CompareScalarLessThanOrEqual, OP_SSE_CMPSS, CMP_LE},
	{SN_CompareScalarNotEqual, OP_SSE_CMPSS, CMP_NE},
	{SN_CompareScalarNotGreaterThan, OP_SSE_CMPSS, CMP_LE},
	{SN_CompareScalarNotGreaterThanOrEqual, OP_SSE_CMPSS, CMP_LT},
	{SN_CompareScalarNotLessThan, OP_SSE_CMPSS, CMP_GE},
	{SN_CompareScalarNotLessThanOrEqual, OP_SSE_CMPSS, CMP_GT},
	{SN_CompareScalarOrdered, OP_SSE_CMPSS, CMP_ORD},
	{SN_CompareScalarOrderedEqual, OP_SSE_COMISS, CMP_EQ},
	{SN_CompareScalarOrderedGreaterThan, OP_SSE_COMISS, CMP_GT},
	{SN_CompareScalarOrderedGreaterThanOrEqual, OP_SSE_COMISS, CMP_GE},
	{SN_CompareScalarOrderedLessThan, OP_SSE_COMISS, CMP_LT},
	{SN_CompareScalarOrderedLessThanOrEqual, OP_SSE_COMISS, CMP_LE},
	{SN_CompareScalarOrderedNotEqual, OP_SSE_COMISS, CMP_NE},
	{SN_CompareScalarUnordered, OP_SSE_CMPSS, CMP_UNORD},
	{SN_CompareScalarUnorderedEqual, OP_SSE_UCOMISS, CMP_EQ},
	{SN_CompareScalarUnorderedGreaterThan, OP_SSE_UCOMISS, CMP_GT},
	{SN_CompareScalarUnorderedGreaterThanOrEqual, OP_SSE_UCOMISS, CMP_GE},
	{SN_CompareScalarUnorderedLessThan, OP_SSE_UCOMISS, CMP_LT},
	{SN_CompareScalarUnorderedLessThanOrEqual, OP_SSE_UCOMISS, CMP_LE},
	{SN_CompareScalarUnorderedNotEqual, OP_SSE_UCOMISS, CMP_NE},
	{SN_CompareUnordered, OP_XCOMPARE_FP, CMP_UNORD},
	{SN_ConvertScalarToVector128Single},
	{SN_ConvertToInt32, OP_XOP_I4_X, SIMD_OP_SSE_CVTSS2SI},
	{SN_ConvertToInt32WithTruncation, OP_XOP_I4_X, SIMD_OP_SSE_CVTTSS2SI},
	{SN_ConvertToInt64, OP_XOP_I8_X, SIMD_OP_SSE_CVTSS2SI64},
	{SN_ConvertToInt64WithTruncation, OP_XOP_I8_X, SIMD_OP_SSE_CVTTSS2SI64},
	{SN_Divide, OP_XBINOP, OP_FDIV},
	{SN_DivideScalar, OP_SSE_DIVSS},
	{SN_LoadAlignedVector128, OP_SSE_LOADU, 16 /* alignment */},
	{SN_LoadHigh, OP_SSE_MOVHPS_LOAD},
	{SN_LoadLow, OP_SSE_MOVLPS_LOAD},
	{SN_LoadScalarVector128, OP_SSE_MOVSS},
	{SN_LoadVector128, OP_SSE_LOADU, 1 /* alignment */},
	{SN_Max, OP_XOP_X_X_X, SIMD_OP_SSE_MAXPS},
	{SN_MaxScalar, OP_XOP_X_X_X, SIMD_OP_SSE_MAXSS},
	{SN_Min, OP_XOP_X_X_X, SIMD_OP_SSE_MINPS},
	{SN_MinScalar, OP_XOP_X_X_X, SIMD_OP_SSE_MINSS},
	{SN_MoveHighToLow, OP_SSE_MOVEHL},
	{SN_MoveLowToHigh, OP_SSE_MOVELH},
	{SN_MoveMask, OP_SSE_MOVMSK},
	{SN_MoveScalar, OP_SSE_MOVS2},
	{SN_Multiply, OP_XBINOP, OP_FMUL},
	{SN_MultiplyScalar, OP_SSE_MULSS},
	{SN_Or, OP_SSE_OR},
	{SN_Prefetch0, OP_SSE_PREFETCHT0},
	{SN_Prefetch1, OP_SSE_PREFETCHT1},
	{SN_Prefetch2, OP_SSE_PREFETCHT2},
	{SN_PrefetchNonTemporal, OP_SSE_PREFETCHNTA},
	{SN_Reciprocal, OP_XOP_X_X, SIMD_OP_SSE_RCPPS},
	{SN_ReciprocalScalar},
	{SN_ReciprocalSqrt, OP_XOP_X_X, SIMD_OP_SSE_RSQRTPS},
	{SN_ReciprocalSqrtScalar},
	{SN_Shuffle},
	{SN_Sqrt, OP_XOP_X_X, SIMD_OP_SSE_SQRTPS},
	{SN_SqrtScalar},
	{SN_Store, OP_SSE_STORE, 1 /* alignment */},
	{SN_StoreAligned, OP_SSE_STORE, 16 /* alignment */},
	{SN_StoreAlignedNonTemporal, OP_SSE_MOVNTPS, 16 /* alignment */},
	{SN_StoreFence, OP_XOP, SIMD_OP_SSE_SFENCE},
	{SN_StoreHigh, OP_SSE_MOVHPS_STORE},
	{SN_StoreLow, OP_SSE_MOVLPS_STORE},
	{SN_StoreScalar, OP_SSE_MOVSS_STORE},
	{SN_Subtract, OP_XBINOP, OP_FSUB},
	{SN_SubtractScalar, OP_SSE_SUBSS},
	{SN_UnpackHigh, OP_SSE_UNPACKHI},
	{SN_UnpackLow, OP_SSE_UNPACKLO},
	{SN_Xor, OP_XBINOP_COERCE, XBINOP_Xor},
	{SN_get_IsSupported}
};

static SimdIntrinsic sse2_methods [] = {
	{SN_Add},
	{SN_AddSaturate, OP_SSE2_ADDS},
	{SN_AddScalar, OP_SSE2_ADDSD},
	{SN_And, OP_SSE_AND},
	{SN_AndNot, OP_SSE_ANDN},
	{SN_Average},
	{SN_CompareEqual},
	{SN_CompareGreaterThan},
	{SN_CompareGreaterThanOrEqual, OP_XCOMPARE_FP, CMP_GE},
	{SN_CompareLessThan},
	{SN_CompareLessThanOrEqual, OP_XCOMPARE_FP, CMP_LE},
	{SN_CompareNotEqual, OP_XCOMPARE_FP, CMP_NE},
	{SN_CompareNotGreaterThan, OP_XCOMPARE_FP, CMP_LE},
	{SN_CompareNotGreaterThanOrEqual, OP_XCOMPARE_FP, CMP_LT},
	{SN_CompareNotLessThan, OP_XCOMPARE_FP, CMP_GE},
	{SN_CompareNotLessThanOrEqual, OP_XCOMPARE_FP, CMP_GT},
	{SN_CompareOrdered, OP_XCOMPARE_FP, CMP_ORD},
	{SN_CompareScalarEqual, OP_SSE2_CMPSD, CMP_EQ},
	{SN_CompareScalarGreaterThan, OP_SSE2_CMPSD, CMP_GT},
	{SN_CompareScalarGreaterThanOrEqual, OP_SSE2_CMPSD, CMP_GE},
	{SN_CompareScalarLessThan, OP_SSE2_CMPSD, CMP_LT},
	{SN_CompareScalarLessThanOrEqual, OP_SSE2_CMPSD, CMP_LE},
	{SN_CompareScalarNotEqual, OP_SSE2_CMPSD, CMP_NE},
	{SN_CompareScalarNotGreaterThan, OP_SSE2_CMPSD, CMP_LE},
	{SN_CompareScalarNotGreaterThanOrEqual, OP_SSE2_CMPSD, CMP_LT},
	{SN_CompareScalarNotLessThan, OP_SSE2_CMPSD, CMP_GE},
	{SN_CompareScalarNotLessThanOrEqual, OP_SSE2_CMPSD, CMP_GT},
	{SN_CompareScalarOrdered, OP_SSE2_CMPSD, CMP_ORD},
	{SN_CompareScalarOrderedEqual, OP_SSE2_COMISD, CMP_EQ},
	{SN_CompareScalarOrderedGreaterThan, OP_SSE2_COMISD, CMP_GT},
	{SN_CompareScalarOrderedGreaterThanOrEqual, OP_SSE2_COMISD, CMP_GE},
	{SN_CompareScalarOrderedLessThan, OP_SSE2_COMISD, CMP_LT},
	{SN_CompareScalarOrderedLessThanOrEqual, OP_SSE2_COMISD, CMP_LE},
	{SN_CompareScalarOrderedNotEqual, OP_SSE2_COMISD, CMP_NE},
	{SN_CompareScalarUnordered, OP_SSE2_CMPSD, CMP_UNORD},
	{SN_CompareScalarUnorderedEqual, OP_SSE2_UCOMISD, CMP_EQ},
	{SN_CompareScalarUnorderedGreaterThan, OP_SSE2_UCOMISD, CMP_GT},
	{SN_CompareScalarUnorderedGreaterThanOrEqual, OP_SSE2_UCOMISD, CMP_GE},
	{SN_CompareScalarUnorderedLessThan, OP_SSE2_UCOMISD, CMP_LT},
	{SN_CompareScalarUnorderedLessThanOrEqual, OP_SSE2_UCOMISD, CMP_LE},
	{SN_CompareScalarUnorderedNotEqual, OP_SSE2_UCOMISD, CMP_NE},
	{SN_CompareUnordered, OP_XCOMPARE_FP, CMP_UNORD},
	{SN_ConvertScalarToVector128Double},
	{SN_ConvertScalarToVector128Int32},
	{SN_ConvertScalarToVector128Int64},
	{SN_ConvertScalarToVector128Single, OP_XOP_X_X_X, SIMD_OP_SSE_CVTSD2SS},
	{SN_ConvertScalarToVector128UInt32},
	{SN_ConvertScalarToVector128UInt64},
	{SN_ConvertToInt32},
	{SN_ConvertToInt32WithTruncation, OP_XOP_I4_X, SIMD_OP_SSE_CVTTSD2SI},
	{SN_ConvertToInt64},
	{SN_ConvertToInt64WithTruncation, OP_XOP_I8_X, SIMD_OP_SSE_CVTTSD2SI64},
	{SN_ConvertToUInt32},
	{SN_ConvertToUInt64},
	{SN_ConvertToVector128Double},
	{SN_ConvertToVector128Int32},
	{SN_ConvertToVector128Int32WithTruncation},
	{SN_ConvertToVector128Single},
	{SN_Divide, OP_XBINOP, OP_FDIV},
	{SN_DivideScalar, OP_SSE2_DIVSD},
	{SN_Extract},
	{SN_Insert},
	{SN_LoadAlignedVector128},
	{SN_LoadFence, OP_XOP, SIMD_OP_SSE_LFENCE},
	{SN_LoadHigh, OP_SSE2_MOVHPD_LOAD},
	{SN_LoadLow, OP_SSE2_MOVLPD_LOAD},
	{SN_LoadScalarVector128},
	{SN_LoadVector128},
	{SN_MaskMove, OP_SSE2_MASKMOVDQU},
	{SN_Max},
	{SN_MaxScalar, OP_XOP_X_X_X, SIMD_OP_SSE_MAXSD},
	{SN_MemoryFence, OP_XOP, SIMD_OP_SSE_MFENCE},
	{SN_Min}, // FIXME:
	{SN_MinScalar, OP_XOP_X_X_X, SIMD_OP_SSE_MINSD},
	{SN_MoveMask, OP_SSE_MOVMSK},
	{SN_MoveScalar},
	{SN_Multiply},
	{SN_MultiplyAddAdjacent, OP_XOP_X_X_X, SIMD_OP_SSE_PMADDWD},
	{SN_MultiplyHigh},
	{SN_MultiplyLow, OP_PMULW},
	{SN_MultiplyScalar, OP_SSE2_MULSD},
	{SN_Or, OP_SSE_OR},
	{SN_PackSignedSaturate},
	{SN_PackUnsignedSaturate},
	{SN_ShiftLeftLogical},
	{SN_ShiftLeftLogical128BitLane},
	{SN_ShiftRightArithmetic},
	{SN_ShiftRightLogical},
	{SN_ShiftRightLogical128BitLane},
	{SN_Shuffle},
	{SN_ShuffleHigh},
	{SN_ShuffleLow},
	{SN_Sqrt, OP_XOP_X_X, SIMD_OP_SSE_SQRTPD},
	{SN_SqrtScalar},
	{SN_Store, OP_SSE_STORE, 1 /* alignment */},
	{SN_StoreAligned, OP_SSE_STORE, 16 /* alignment */},
	{SN_StoreAlignedNonTemporal, OP_SSE_MOVNTPS, 16 /* alignment */},
	{SN_StoreHigh, OP_SSE2_MOVHPD_STORE},
	{SN_StoreLow, OP_SSE2_MOVLPD_STORE},
	{SN_StoreNonTemporal, OP_SSE_MOVNTPS, 1 /* alignment */},
	{SN_StoreScalar, OP_SSE_STORES},
	{SN_Subtract},
	{SN_SubtractSaturate, OP_SSE2_SUBS},
	{SN_SubtractScalar, OP_SSE2_SUBSD},
	{SN_SumAbsoluteDifferences, OP_XOP_X_X_X, SIMD_OP_SSE_PSADBW},
	{SN_UnpackHigh, OP_SSE_UNPACKHI},
	{SN_UnpackLow, OP_SSE_UNPACKLO},
	{SN_Xor, OP_XBINOP_COERCE, XBINOP_Xor},
	{SN_get_IsSupported}
};

static SimdIntrinsic sse3_methods [] = {
	{SN_AddSubtract},
	{SN_HorizontalAdd},
	{SN_HorizontalSubtract},
	{SN_LoadAndDuplicateToVector128, OP_SSE3_MOVDDUP_MEM},
	{SN_LoadDquVector128, OP_XOP_X_I, SIMD_OP_SSE_LDDQU},
	{SN_MoveAndDuplicate, OP_SSE3_MOVDDUP},
	{SN_MoveHighAndDuplicate, OP_SSE3_MOVSHDUP},
	{SN_MoveLowAndDuplicate, OP_SSE3_MOVSLDUP},
	{SN_get_IsSupported}
};

static SimdIntrinsic ssse3_methods [] = {
	{SN_Abs, OP_SSSE3_ABS},
	{SN_AlignRight},
	{SN_HorizontalAdd},
	{SN_HorizontalAddSaturate, OP_XOP_X_X_X, SIMD_OP_SSE_PHADDSW},
	{SN_HorizontalSubtract},
	{SN_HorizontalSubtractSaturate, OP_XOP_X_X_X, SIMD_OP_SSE_PHSUBSW},
	{SN_MultiplyAddAdjacent, OP_XOP_X_X_X, SIMD_OP_SSE_PMADDUBSW},
	{SN_MultiplyHighRoundScale, OP_XOP_X_X_X, SIMD_OP_SSE_PMULHRSW},
	{SN_Shuffle, OP_SSSE3_SHUFFLE},
	{SN_Sign},
	{SN_get_IsSupported}
};

static SimdIntrinsic sse41_methods [] = {
	{SN_Blend},
	{SN_BlendVariable},
	{SN_Ceiling, OP_SSE41_ROUNDP, 10 /*round mode*/},
	{SN_CeilingScalar, 0, 10 /*round mode*/},
	{SN_CompareEqual, OP_XCOMPARE, CMP_EQ},
	{SN_ConvertToVector128Int16, OP_SSE_CVTII, MONO_TYPE_I2},
	{SN_ConvertToVector128Int32, OP_SSE_CVTII, MONO_TYPE_I4},
	{SN_ConvertToVector128Int64, OP_SSE_CVTII, MONO_TYPE_I8},
	{SN_DotProduct},
	{SN_Extract},
	{SN_Floor, OP_SSE41_ROUNDP, 9 /*round mode*/},
	{SN_FloorScalar, 0, 9 /*round mode*/},
	{SN_Insert},
	{SN_LoadAlignedVector128NonTemporal, OP_SSE41_LOADANT},
	{SN_Max, OP_XBINOP, OP_IMAX},
	{SN_Min, OP_XBINOP, OP_IMIN},
	{SN_MinHorizontal, OP_XOP_X_X, SIMD_OP_SSE_PHMINPOSUW},
	{SN_MultipleSumAbsoluteDifferences},
	{SN_Multiply, OP_SSE41_MUL},
	{SN_MultiplyLow, OP_SSE41_MULLO},
	{SN_PackUnsignedSaturate, OP_XOP_X_X_X, SIMD_OP_SSE_PACKUSDW},
	{SN_RoundCurrentDirection, OP_SSE41_ROUNDP, 4 /*round mode*/},
	{SN_RoundCurrentDirectionScalar, 0, 4 /*round mode*/},
	{SN_RoundToNearestInteger, OP_SSE41_ROUNDP, 8 /*round mode*/},
	{SN_RoundToNearestIntegerScalar, 0, 8 /*round mode*/},
	{SN_RoundToNegativeInfinity, OP_SSE41_ROUNDP, 9 /*round mode*/},
	{SN_RoundToNegativeInfinityScalar, 0, 9 /*round mode*/},
	{SN_RoundToPositiveInfinity, OP_SSE41_ROUNDP, 10 /*round mode*/},
	{SN_RoundToPositiveInfinityScalar, 0, 10 /*round mode*/},
	{SN_RoundToZero, OP_SSE41_ROUNDP, 11 /*round mode*/},
	{SN_RoundToZeroScalar, 0, 11 /*round mode*/},
	{SN_TestC, OP_XOP_I4_X_X, SIMD_OP_SSE_TESTC},
	{SN_TestNotZAndNotC, OP_XOP_I4_X_X, SIMD_OP_SSE_TESTNZ},
	{SN_TestZ, OP_XOP_I4_X_X, SIMD_OP_SSE_TESTZ},
	{SN_get_IsSupported}
};

static SimdIntrinsic sse42_methods [] = {
	{SN_CompareGreaterThan, OP_XCOMPARE, CMP_GT},
	{SN_Crc32},
	{SN_get_IsSupported}
};

static SimdIntrinsic pclmulqdq_methods [] = {
	{SN_CarrylessMultiply},
	{SN_get_IsSupported}
};

static SimdIntrinsic aes_methods [] = {
	{SN_Decrypt, OP_XOP_X_X_X, SIMD_OP_AES_DEC},
	{SN_DecryptLast, OP_XOP_X_X_X, SIMD_OP_AES_DECLAST},
	{SN_Encrypt, OP_XOP_X_X_X, SIMD_OP_AES_ENC},
	{SN_EncryptLast, OP_XOP_X_X_X, SIMD_OP_AES_ENCLAST},
	{SN_InverseMixColumns, OP_XOP_X_X, SIMD_OP_AES_IMC},
	{SN_KeygenAssist},
	{SN_get_IsSupported}
};

static SimdIntrinsic popcnt_methods [] = {
	{SN_PopCount},
	{SN_get_IsSupported}
};

static SimdIntrinsic lzcnt_methods [] = {
	{SN_LeadingZeroCount},
	{SN_get_IsSupported}
};

static SimdIntrinsic bmi1_methods [] = {
	{SN_AndNot},
	{SN_BitFieldExtract},
	{SN_ExtractLowestSetBit},
	{SN_GetMaskUpToLowestSetBit},
	{SN_ResetLowestSetBit},
	{SN_TrailingZeroCount},
	{SN_get_IsSupported}
};

static SimdIntrinsic bmi2_methods [] = {
	{SN_MultiplyNoFlags},
	{SN_ParallelBitDeposit},
	{SN_ParallelBitExtract},
	{SN_ZeroHighBits},
	{SN_get_IsSupported}
};

static SimdIntrinsic x86base_methods [] = {
	{SN_BitScanForward},
	{SN_BitScanReverse},
	{SN_get_IsSupported}
};

static const IntrinGroup supported_x86_intrinsics [] = {
	{ "Aes", MONO_CPU_X86_AES, aes_methods, sizeof (aes_methods) },
	{ "Avx", MONO_CPU_X86_AVX, unsupported, sizeof (unsupported) },
	{ "Avx2", MONO_CPU_X86_AVX2, unsupported, sizeof (unsupported) },
	{ "Bmi1", MONO_CPU_X86_BMI1, bmi1_methods, sizeof (bmi1_methods) },
	{ "Bmi2", MONO_CPU_X86_BMI2, bmi2_methods, sizeof (bmi2_methods) },
	{ "Fma", MONO_CPU_X86_FMA, unsupported, sizeof (unsupported) },
	{ "Lzcnt", MONO_CPU_X86_LZCNT, lzcnt_methods, sizeof (lzcnt_methods), TRUE },
	{ "Pclmulqdq", MONO_CPU_X86_PCLMUL, pclmulqdq_methods, sizeof (pclmulqdq_methods) },
	{ "Popcnt", MONO_CPU_X86_POPCNT, popcnt_methods, sizeof (popcnt_methods), TRUE },
	{ "Sse", MONO_CPU_X86_SSE, sse_methods, sizeof (sse_methods) },
	{ "Sse2", MONO_CPU_X86_SSE2, sse2_methods, sizeof (sse2_methods) },
	{ "Sse3", MONO_CPU_X86_SSE3, sse3_methods, sizeof (sse3_methods) },
	{ "Sse41", MONO_CPU_X86_SSE41, sse41_methods, sizeof (sse41_methods) },
	{ "Sse42", MONO_CPU_X86_SSE42, sse42_methods, sizeof (sse42_methods) },
	{ "Ssse3", MONO_CPU_X86_SSSE3, ssse3_methods, sizeof (ssse3_methods) },
	{ "X86Base", 0, x86base_methods, sizeof (x86base_methods) },
};

static MonoInst*
emit_x86_intrinsics (
	MonoCompile *cfg, MonoMethodSignature *fsig, MonoInst **args,
	MonoClass *klass, const IntrinGroup *intrin_group,
	const SimdIntrinsic *info, int id, MonoTypeEnum arg0_type,
	gboolean is_64bit)
{
	MonoCPUFeatures feature = intrin_group->feature;
	const SimdIntrinsic *intrinsics = intrin_group->intrinsics;

	if (feature == MONO_CPU_X86_SSE) {
		switch (id) {
		case SN_Shuffle:
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE_SHUFFLE, args [2]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant mask (generate a switch)
			return emit_invalid_operation (cfg, "mask in Sse.Shuffle must be constant");
		case SN_ConvertScalarToVector128Single: {
			int op = 0;
			switch (fsig->params [1]->type) {
			case MONO_TYPE_I4: op = OP_SSE_CVTSI2SS; break;
			case MONO_TYPE_I8: op = OP_SSE_CVTSI2SS64; break;
			default: g_assert_not_reached (); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, op, 0, 0, fsig, args);
		}
		case SN_ReciprocalScalar:
		case SN_ReciprocalSqrtScalar:
		case SN_SqrtScalar: {
			int op = 0;
			switch (id) {
			case SN_ReciprocalScalar: op = OP_SSE_RCPSS; break;
			case SN_ReciprocalSqrtScalar: op = OP_SSE_RSQRTSS; break;
			case SN_SqrtScalar: op = OP_SSE_SQRTSS; break;
			};
			if (fsig->param_count == 1)
				return emit_simd_ins (cfg, klass, op, args [0]->dreg, args[0]->dreg);
			else if (fsig->param_count == 2)
				return emit_simd_ins (cfg, klass, op, args [0]->dreg, args[1]->dreg);
			else 
				g_assert_not_reached ();
			break;
		}
		case SN_LoadScalarVector128:
			return NULL;
		default:
			return NULL;
		}
	}

	if (feature == MONO_CPU_X86_SSE2) {
		switch (id) {
		case SN_Subtract:
			return emit_simd_ins_for_sig (cfg, klass, OP_XBINOP, arg0_type == MONO_TYPE_R8 ? OP_FSUB : OP_ISUB, arg0_type, fsig, args);
		case SN_Add:
			return emit_simd_ins_for_sig (cfg, klass, OP_XBINOP, arg0_type == MONO_TYPE_R8 ? OP_FADD : OP_IADD, arg0_type, fsig, args);
		case SN_Average:
			if (arg0_type == MONO_TYPE_U1)
				return emit_simd_ins_for_sig (cfg, klass, OP_PAVGB_UN, -1, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_U2)
				return emit_simd_ins_for_sig (cfg, klass, OP_PAVGW_UN, -1, arg0_type, fsig, args);
			else
				return NULL;
		case SN_CompareNotEqual:
			return emit_simd_ins_for_sig (cfg, klass, arg0_type == MONO_TYPE_R8 ? OP_XCOMPARE_FP : OP_XCOMPARE, CMP_NE, arg0_type, fsig, args);
		case SN_CompareEqual:
			return emit_simd_ins_for_sig (cfg, klass, arg0_type == MONO_TYPE_R8 ? OP_XCOMPARE_FP : OP_XCOMPARE, CMP_EQ, arg0_type, fsig, args);
		case SN_CompareGreaterThan:
			return emit_simd_ins_for_sig (cfg, klass, arg0_type == MONO_TYPE_R8 ? OP_XCOMPARE_FP : OP_XCOMPARE, CMP_GT, arg0_type, fsig, args);
		case SN_CompareLessThan:
			return emit_simd_ins_for_sig (cfg, klass, arg0_type == MONO_TYPE_R8 ? OP_XCOMPARE_FP : OP_XCOMPARE, CMP_LT, arg0_type, fsig, args);
		case SN_ConvertToInt32:
			if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_I4_X, SIMD_OP_SSE_CVTSD2SI, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_I4)
				return emit_simd_ins_for_sig (cfg, klass, OP_EXTRACT_I4, 0, arg0_type, fsig, args);
			else
				return NULL;
		case SN_ConvertToInt64:
			if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_I8_X, SIMD_OP_SSE_CVTSD2SI64, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_I8)
				return emit_simd_ins_for_sig (cfg, klass, OP_EXTRACT_I8, 0 /*element index*/, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
			break;
		case SN_ConvertScalarToVector128Double: {
			int op = OP_SSE2_CVTSS2SD;
			switch (fsig->params [1]->type) {
			case MONO_TYPE_I4: op = OP_SSE2_CVTSI2SD; break;
			case MONO_TYPE_I8: op = OP_SSE2_CVTSI2SD64; break;
			}
			return emit_simd_ins_for_sig (cfg, klass, op, 0, 0, fsig, args);
		}
		case SN_ConvertScalarToVector128Int32:
		case SN_ConvertScalarToVector128Int64:
		case SN_ConvertScalarToVector128UInt32:
		case SN_ConvertScalarToVector128UInt64:
			return emit_simd_ins_for_sig (cfg, klass, OP_CREATE_SCALAR, -1, arg0_type, fsig, args);
		case SN_ConvertToUInt32:
			return emit_simd_ins_for_sig (cfg, klass, OP_EXTRACT_I4, 0 /*element index*/, arg0_type, fsig, args);
		case SN_ConvertToUInt64:
			return emit_simd_ins_for_sig (cfg, klass, OP_EXTRACT_I8, 0 /*element index*/, arg0_type, fsig, args);
		case SN_ConvertToVector128Double:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTPS2PD, 0, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_I4)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTDQ2PD, 0, arg0_type, fsig, args);
			else
				return NULL;
		case SN_ConvertToVector128Int32:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTPS2DQ, 0, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTPD2DQ, 0, arg0_type, fsig, args);
			else
				return NULL;
		case SN_ConvertToVector128Int32WithTruncation:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTTPS2DQ, 0, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTTPD2DQ, 0, arg0_type, fsig, args);
			else
				return NULL;
		case SN_ConvertToVector128Single:
			if (arg0_type == MONO_TYPE_I4)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTDQ2PS, 0, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_CVTPD2PS, 0, arg0_type, fsig, args);
			else
				return NULL;
		case SN_LoadAlignedVector128:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE_LOADU, 16 /*alignment*/, arg0_type, fsig, args);
		case SN_LoadVector128:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE_LOADU, 1 /*alignment*/, arg0_type, fsig, args);
		case SN_MoveScalar:
			return emit_simd_ins_for_sig (cfg, klass, fsig->param_count == 2 ? OP_SSE_MOVS2 : OP_SSE_MOVS, -1, arg0_type, fsig, args);
		case SN_Max:
			switch (arg0_type) {
			case MONO_TYPE_U1:
				return emit_simd_ins_for_sig (cfg, klass, OP_PMAXB_UN, 0, arg0_type, fsig, args);
			case MONO_TYPE_I2:
				return emit_simd_ins_for_sig (cfg, klass, OP_PMAXW, 0, arg0_type, fsig, args);
			case MONO_TYPE_R8: return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_MAXPD, arg0_type, fsig, args);
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		case SN_Min:
			switch (arg0_type) {
			case MONO_TYPE_U1:
				return emit_simd_ins_for_sig (cfg, klass, OP_PMINB_UN, 0, arg0_type, fsig, args);
			case MONO_TYPE_I2:
				return emit_simd_ins_for_sig (cfg, klass, OP_PMINW, 0, arg0_type, fsig, args);
			case MONO_TYPE_R8: return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_MINPD, arg0_type, fsig, args);
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		case SN_Multiply:
			if (arg0_type == MONO_TYPE_U4)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PMULUDQ, 0, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_MULPD, 0, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
		case SN_MultiplyHigh:
			if (arg0_type == MONO_TYPE_I2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PMULHW, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_U2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PMULHUW, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
		case SN_PackSignedSaturate:
			if (arg0_type == MONO_TYPE_I2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PACKSSWB, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_I4)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PACKSSDW, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
		case SN_PackUnsignedSaturate:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PACKUS, -1, arg0_type, fsig, args);
		case SN_Extract:
			g_assert (arg0_type == MONO_TYPE_U2);
			return emit_simd_ins_for_sig (cfg, klass, OP_XEXTRACT_I32, arg0_type, 0, fsig, args);
		case SN_Insert:
			g_assert (arg0_type == MONO_TYPE_I2 || arg0_type == MONO_TYPE_U2);
			return emit_simd_ins_for_sig (cfg, klass, OP_XINSERT_I2, 0, arg0_type, fsig, args);
		case SN_ShiftRightLogical: {
			gboolean is_imm = fsig->params [1]->type == MONO_TYPE_U1;
			SimdOp op = (SimdOp)0;
			switch (arg0_type) {
			case MONO_TYPE_I2:
			case MONO_TYPE_U2: 
				op = is_imm ? SIMD_OP_SSE_PSRLW_IMM : SIMD_OP_SSE_PSRLW; 
				break;
			case MONO_TYPE_I4:
			case MONO_TYPE_U4: 
				op = is_imm ? SIMD_OP_SSE_PSRLD_IMM : SIMD_OP_SSE_PSRLD; 
				break;
			case MONO_TYPE_I8:
			case MONO_TYPE_U8: 
				op = is_imm ? SIMD_OP_SSE_PSRLQ_IMM : SIMD_OP_SSE_PSRLQ; 
				break;
			default: g_assert_not_reached (); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, is_imm ? OP_XOP_X_X_I4 : OP_XOP_X_X_X, op, arg0_type, fsig, args);
		}
		case SN_ShiftRightArithmetic: {
			gboolean is_imm = fsig->params [1]->type == MONO_TYPE_U1;
			SimdOp op = (SimdOp)0;
			switch (arg0_type) {
			case MONO_TYPE_I2:
			case MONO_TYPE_U2: 
				op = is_imm ? SIMD_OP_SSE_PSRAW_IMM : SIMD_OP_SSE_PSRAW; 
				break;
			case MONO_TYPE_I4:
			case MONO_TYPE_U4: 
				op = is_imm ? SIMD_OP_SSE_PSRAD_IMM : SIMD_OP_SSE_PSRAD; 
				break;
			default: g_assert_not_reached (); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, is_imm ? OP_XOP_X_X_I4 : OP_XOP_X_X_X, op, arg0_type, fsig, args);
		}
		case SN_ShiftLeftLogical: {
			gboolean is_imm = fsig->params [1]->type == MONO_TYPE_U1;
			SimdOp op = (SimdOp)0;
			switch (arg0_type) {
			case MONO_TYPE_I2:
			case MONO_TYPE_U2: 
				op = is_imm ? SIMD_OP_SSE_PSLLW_IMM : SIMD_OP_SSE_PSLLW; 
				break;
			case MONO_TYPE_I4:
			case MONO_TYPE_U4: 
				op = is_imm ? SIMD_OP_SSE_PSLLD_IMM : SIMD_OP_SSE_PSLLD; 
				break;
			case MONO_TYPE_I8:
			case MONO_TYPE_U8: 
				op = is_imm ? SIMD_OP_SSE_PSLLQ_IMM : SIMD_OP_SSE_PSLLQ; 
				break;
			default: g_assert_not_reached (); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, is_imm ? OP_XOP_X_X_I4 : OP_XOP_X_X_X, op, arg0_type, fsig, args);
		}
		case SN_ShiftLeftLogical128BitLane:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PSLLDQ, 0, arg0_type, fsig, args);
		case SN_ShiftRightLogical128BitLane:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PSRLDQ, 0, arg0_type, fsig, args);
		case SN_Shuffle: {
			if (fsig->param_count == 2) {
				g_assert (arg0_type == MONO_TYPE_I4 || arg0_type == MONO_TYPE_U4);
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PSHUFD, 0, arg0_type, fsig, args);
			} else if (fsig->param_count == 3) {
				g_assert (arg0_type == MONO_TYPE_R8);
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_SHUFPD, 0, arg0_type, fsig, args);
			} else {
				g_assert_not_reached ();
				break;
			}
		}
		case SN_ShuffleHigh:
			g_assert (fsig->param_count == 2);
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PSHUFHW, 0, arg0_type, fsig, args);
		case SN_ShuffleLow:
			g_assert (fsig->param_count == 2);
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE2_PSHUFLW, 0, arg0_type, fsig, args);
		case SN_SqrtScalar: {
			if (fsig->param_count == 1)
				return emit_simd_ins (cfg, klass, OP_SSE2_SQRTSD, args [0]->dreg, args[0]->dreg);
			else if (fsig->param_count == 2)
				return emit_simd_ins (cfg, klass, OP_SSE2_SQRTSD, args [0]->dreg, args[1]->dreg);
			else {
				g_assert_not_reached ();
				break;
			}
		}
		case SN_LoadScalarVector128: {
			int op = 0;
			switch (arg0_type) {
			case MONO_TYPE_I4:
			case MONO_TYPE_U4: op = OP_SSE2_MOVD; break;
			case MONO_TYPE_I8:
			case MONO_TYPE_U8: op = OP_SSE2_MOVQ; break;
			case MONO_TYPE_R8: op = OP_SSE2_MOVUPD; break;
			default: g_assert_not_reached(); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, op, 0, 0, fsig, args);
		}
		default:
			return NULL;
		}
	}

	if (feature == MONO_CPU_X86_SSE3) {
		switch (id) {
		case SN_AddSubtract:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_ADDSUBPS, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_ADDSUBPD, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
			break;
		case SN_HorizontalAdd:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_HADDPS, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_HADDPD, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
			break;
		case SN_HorizontalSubtract:
			if (arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_HSUBPS, arg0_type, fsig, args);
			else if (arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_HSUBPD, arg0_type, fsig, args);
			else
				g_assert_not_reached ();
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (feature == MONO_CPU_X86_SSSE3) {
		switch (id) {
		case SN_AlignRight:
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSSE3_ALIGNR, args [2]->inst_c0, arg0_type, fsig, args);
			return emit_invalid_operation (cfg, "mask in Ssse3.AlignRight must be constant");
		case SN_HorizontalAdd:
			if (arg0_type == MONO_TYPE_I2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PHADDW, arg0_type, fsig, args);
			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PHADDD, arg0_type, fsig, args);
		case SN_HorizontalSubtract:
			if (arg0_type == MONO_TYPE_I2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PHSUBW, arg0_type, fsig, args);
			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PHSUBD, arg0_type, fsig, args);
		case SN_Sign:
			if (arg0_type == MONO_TYPE_I1)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PSIGNB, arg0_type, fsig, args);
			if (arg0_type == MONO_TYPE_I2)
				return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PSIGNW, arg0_type, fsig, args);
			return emit_simd_ins_for_sig (cfg, klass, OP_XOP_X_X_X, SIMD_OP_SSE_PSIGND, arg0_type, fsig, args);
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (feature == MONO_CPU_X86_SSE41) {
		switch (id) {
		case SN_DotProduct:
			if (args [2]->opcode == OP_ICONST && arg0_type == MONO_TYPE_R4)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_DPPS_IMM, args [2]->inst_c0, arg0_type, fsig, args);
			else if (args [2]->opcode == OP_ICONST && arg0_type == MONO_TYPE_R8)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_DPPD_IMM, args [2]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant control byte (generate a switch)
			return emit_invalid_operation (cfg, "control byte in Sse41.DotProduct must be constant");
		case SN_MultipleSumAbsoluteDifferences:
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_MPSADBW_IMM, args [2]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant control byte (generate a switch)
			return emit_invalid_operation (cfg, "control byte in Sse41.MultipleSumAbsoluteDifferences must be constant");
		case SN_Blend:
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_BLEND_IMM, args [2]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant control byte (generate a switch)
			return emit_invalid_operation (cfg, "control byte in Sse41.Blend must be constant");
		case SN_BlendVariable:
			return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_BLENDV, -1, arg0_type, fsig, args);
		case SN_Extract: {
			int op = 0;
			switch (arg0_type) {
			case MONO_TYPE_U1:
			case MONO_TYPE_U4:
			case MONO_TYPE_I4: op = OP_XEXTRACT_I32; break;
			case MONO_TYPE_I8:
			case MONO_TYPE_U8: op = OP_XEXTRACT_I64; break;
			case MONO_TYPE_R4: op = OP_XEXTRACT_R4; break;
			default: g_assert_not_reached(); break;
			}
			return emit_simd_ins_for_sig (cfg, klass, op, arg0_type, 0, fsig, args);
		}
		case SN_Insert:
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_INSERT, -1, arg0_type, fsig, args);
			// FIXME: handle non-constant index (generate a switch)
			return emit_invalid_operation (cfg, "index in Sse41.Insert must be constant");
		case SN_CeilingScalar:
		case SN_FloorScalar:
		case SN_RoundCurrentDirectionScalar:
		case SN_RoundToNearestIntegerScalar:
		case SN_RoundToNegativeInfinityScalar:
		case SN_RoundToPositiveInfinityScalar:
		case SN_RoundToZeroScalar:
			if (fsig->param_count == 2) {
				return emit_simd_ins_for_sig (cfg, klass, OP_SSE41_ROUNDS, info->instc0, arg0_type, fsig, args);
			} else {
				MonoInst* ins = emit_simd_ins (cfg, klass, OP_SSE41_ROUNDS, args [0]->dreg, args [0]->dreg);
				ins->inst_c0 = info->instc0;
				ins->inst_c1 = arg0_type;
				return ins;
			}
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (feature == MONO_CPU_X86_SSE42) {
		switch (id) {
		case SN_Crc32: {
			MonoTypeEnum arg1_type = get_underlying_type (fsig->params [1]);
			return emit_simd_ins_for_sig (cfg, klass, 
				arg1_type == MONO_TYPE_U8 ? OP_SSE42_CRC64 : OP_SSE42_CRC32, 
				arg1_type, arg0_type, fsig, args);
		}
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (feature == MONO_CPU_X86_PCLMUL) {
		switch (id) {
		case SN_CarrylessMultiply: {
			if (args [2]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_PCLMULQDQ_IMM, args [2]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant control byte (generate a switch)
			return emit_invalid_operation (cfg, "index in Pclmulqdq.CarrylessMultiply must be constant");
		}
		default:
			g_assert_not_reached ();
			break;
		}
	}

	if (feature == MONO_CPU_X86_AES) {
		switch (id) {
		case SN_KeygenAssist: {
			if (args [1]->opcode == OP_ICONST)
				return emit_simd_ins_for_sig (cfg, klass, OP_AES_KEYGEN_IMM, args [1]->inst_c0, arg0_type, fsig, args);
			// FIXME: handle non-constant control byte (generate a switch)
			return emit_invalid_operation (cfg, "control byte in Aes.KeygenAssist must be constant");
		}
		default:
			g_assert_not_reached ();
			break;
		}
	}

	MonoInst *ins = NULL;
	if (feature == MONO_CPU_X86_POPCNT) {
		switch (id) {
		case SN_PopCount:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_POPCNT64 : OP_POPCNT32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		default:
			return NULL;
		}
	}
	if (feature == MONO_CPU_X86_LZCNT) {
		switch (id) {
		case SN_LeadingZeroCount:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_LZCNT64 : OP_LZCNT32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		default:
			return NULL;
		}
	}
	if (feature == MONO_CPU_X86_BMI1) {
		switch (id) {
		case SN_AndNot: {
			// (a ^ -1) & b
			// LLVM replaces it with `andn`
			int tmp_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			int result_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			EMIT_NEW_BIALU_IMM (cfg, ins, is_64bit ? OP_LXOR_IMM : OP_IXOR_IMM, tmp_reg, args [0]->dreg, -1);
			EMIT_NEW_BIALU (cfg, ins, is_64bit ? OP_LAND : OP_IAND, result_reg, tmp_reg, args [1]->dreg);
			return ins;
		}
		case SN_BitFieldExtract: {
			if (fsig->param_count == 2) {
				MONO_INST_NEW (cfg, ins, is_64bit ? OP_BEXTR64 : OP_BEXTR32);
				ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
				ins->sreg1 = args [0]->dreg;
				ins->sreg2 = args [1]->dreg;
				ins->type = is_64bit ? STACK_I8 : STACK_I4;
				MONO_ADD_INS (cfg->cbb, ins);
				return ins;
			}
		}
		case SN_GetMaskUpToLowestSetBit: {
			// x ^ (x - 1)
			// LLVM replaces it with `blsmsk`
			int tmp_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			int result_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			EMIT_NEW_BIALU_IMM (cfg, ins, is_64bit ? OP_LSUB_IMM : OP_ISUB_IMM, tmp_reg, args [0]->dreg, 1);
			EMIT_NEW_BIALU (cfg, ins, is_64bit ? OP_LXOR : OP_IXOR, result_reg, args [0]->dreg, tmp_reg);
			return ins;
		}
		case SN_ResetLowestSetBit: {
			// x & (x - 1)
			// LLVM replaces it with `blsr`
			int tmp_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			int result_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			EMIT_NEW_BIALU_IMM (cfg, ins, is_64bit ? OP_LSUB_IMM : OP_ISUB_IMM, tmp_reg, args [0]->dreg, 1);
			EMIT_NEW_BIALU (cfg, ins, is_64bit ? OP_LAND : OP_IAND, result_reg, args [0]->dreg, tmp_reg);
			return ins;
		}
		case SN_ExtractLowestSetBit: {
			// x & (0 - x)
			// LLVM replaces it with `blsi`
			int tmp_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			int result_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			int zero_reg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			MONO_EMIT_NEW_ICONST (cfg, zero_reg, 0);
			EMIT_NEW_BIALU (cfg, ins, is_64bit ? OP_LSUB : OP_ISUB, tmp_reg, zero_reg, args [0]->dreg);
			EMIT_NEW_BIALU (cfg, ins, is_64bit ? OP_LAND : OP_IAND, result_reg, args [0]->dreg, tmp_reg);
			return ins;
		}
		case SN_TrailingZeroCount:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_CTTZ64 : OP_CTTZ32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		default:
			g_assert_not_reached ();
		}
	}
	if (feature == MONO_CPU_X86_BMI2) {
		switch (id) {
		case SN_MultiplyNoFlags:
			if (fsig->param_count == 2) {
				MONO_INST_NEW (cfg, ins, is_64bit ? OP_MULX_H64 : OP_MULX_H32);
				ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
				ins->sreg1 = args [0]->dreg;
				ins->sreg2 = args [1]->dreg;
				ins->type = is_64bit ? STACK_I8 : STACK_I4;
				MONO_ADD_INS (cfg->cbb, ins);
			} else if (fsig->param_count == 3) {
				MONO_INST_NEW (cfg, ins, is_64bit ? OP_MULX_HL64 : OP_MULX_HL32);
				ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
				ins->sreg1 = args [0]->dreg;
				ins->sreg2 = args [1]->dreg;
				ins->sreg3 = args [2]->dreg;
				ins->type = is_64bit ? STACK_I8 : STACK_I4;
				MONO_ADD_INS (cfg->cbb, ins);
			} else {
				g_assert_not_reached ();
			}
			return ins;
		case SN_ZeroHighBits:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_BZHI64 : OP_BZHI32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->sreg2 = args [1]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		case SN_ParallelBitExtract:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_PEXT64 : OP_PEXT32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->sreg2 = args [1]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		case SN_ParallelBitDeposit:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_PDEP64 : OP_PDEP32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->sreg2 = args [1]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		default:
			g_assert_not_reached ();
		}
	}

	if (intrinsics == x86base_methods) {
		switch (id) {
		case SN_BitScanForward:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_X86_BSF64 : OP_X86_BSF32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		case SN_BitScanReverse:
			MONO_INST_NEW (cfg, ins, is_64bit ? OP_X86_BSR64 : OP_X86_BSR32);
			ins->dreg = is_64bit ? alloc_lreg (cfg) : alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->type = is_64bit ? STACK_I8 : STACK_I4;
			MONO_ADD_INS (cfg->cbb, ins);
			return ins;
		default:
			g_assert_not_reached ();
		}
	}

	return NULL;
}

static guint16 vector_128_t_methods [] = {
	SN_get_Count,
	SN_get_Zero,
};

static MonoInst*
emit_vector128_t (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *ins;
	MonoType *type, *etype;
	MonoClass *klass;
	int size, len, id;

	id = lookup_intrins (vector_128_t_methods, sizeof (vector_128_t_methods), cmethod);
	if (id == -1)
		return NULL;

	klass = cmethod->klass;
	type = m_class_get_byval_arg (klass);
	etype = mono_class_get_context (klass)->class_inst->type_argv [0];
	size = mono_class_value_size (mono_class_from_mono_type_internal (etype), NULL);
	g_assert (size);
	len = 16 / size;

	if (!MONO_TYPE_IS_PRIMITIVE (etype) || etype->type == MONO_TYPE_CHAR || etype->type == MONO_TYPE_BOOLEAN)
		return NULL;

	if (cfg->verbose_level > 1) {
		char *name = mono_method_full_name (cmethod, TRUE);
		printf ("  SIMD intrinsic %s\n", name);
		g_free (name);
	}

	switch (id) {
	case SN_get_Count:
		if (!(fsig->param_count == 0 && fsig->ret->type == MONO_TYPE_I4))
			break;
		EMIT_NEW_ICONST (cfg, ins, len);
		return ins;
	case SN_get_Zero: {
		return emit_simd_ins (cfg, klass, OP_XZERO, -1, -1);
	}
	default:
		break;
	}

	return NULL;
}

static guint16 vector_256_t_methods [] = {
	SN_get_Count,
};

static MonoInst*
emit_vector256_t (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *ins;
	MonoType *type, *etype;
	MonoClass *klass;
	int size, len, id;

	id = lookup_intrins (vector_256_t_methods, sizeof (vector_256_t_methods), cmethod);
	if (id == -1)
		return NULL;

	klass = cmethod->klass;
	type = m_class_get_byval_arg (klass);
	etype = mono_class_get_context (klass)->class_inst->type_argv [0];
	size = mono_class_value_size (mono_class_from_mono_type_internal (etype), NULL);
	g_assert (size);
	len = 32 / size;

	if (!MONO_TYPE_IS_PRIMITIVE (etype) || etype->type == MONO_TYPE_CHAR || etype->type == MONO_TYPE_BOOLEAN)
		return NULL;

	if (cfg->verbose_level > 1) {
		char *name = mono_method_full_name (cmethod, TRUE);
		printf ("  SIMD intrinsic %s\n", name);
		g_free (name);
	}

	switch (id) {
	case SN_get_Count:
		if (!(fsig->param_count == 0 && fsig->ret->type == MONO_TYPE_I4))
			break;
		EMIT_NEW_ICONST (cfg, ins, len);
		return ins;
	default:
		break;
	}

	return NULL;
}

static
MonoInst*
emit_amd64_intrinsics (const char *class_ns, const char *class_name, MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	if (!strcmp (class_ns, "System.Runtime.Intrinsics.X86")) {
		return emit_hardware_intrinsics (cfg, cmethod, fsig, args,
			supported_x86_intrinsics, sizeof (supported_x86_intrinsics),
			emit_x86_intrinsics);
	}

	if (!strcmp (class_ns, "System.Runtime.Intrinsics")) {
		if (!strcmp (class_name, "Vector128`1"))
			return emit_vector128_t (cfg, cmethod, fsig, args);
		if (!strcmp (class_name, "Vector256`1"))
			return emit_vector256_t (cfg, cmethod, fsig, args);
	}

	if (!strcmp (class_ns, "System.Numerics")) {
		if (!strcmp (class_name, "Vector"))
			return emit_sys_numerics_vector (cfg, cmethod, fsig, args);
		if (!strcmp (class_name, "Vector`1"))
			return emit_sys_numerics_vector_t (cfg, cmethod, fsig, args);
	}

	return NULL;
}
#endif // !TARGET_ARM64

#ifdef TARGET_ARM64
static
MonoInst*
emit_simd_intrinsics (const char *class_ns, const char *class_name, MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	// FIXME: implement Vector64<T>, Vector128<T> and Vector<T> for Arm64
	if (!strcmp (class_ns, "System.Runtime.Intrinsics.Arm")) {
		return emit_hardware_intrinsics(cfg, cmethod, fsig, args,
			supported_arm_intrinsics, sizeof (supported_arm_intrinsics),
			emit_arm64_intrinsics);
	}

	return NULL;
}
#elif TARGET_AMD64
// TODO: test and enable for x86 too
static
MonoInst*
emit_simd_intrinsics (const char *class_ns, const char *class_name, MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *simd_inst = emit_amd64_intrinsics (class_ns, class_name, cfg, cmethod, fsig, args);
	if (simd_inst != NULL) {
		cfg->uses_simd_intrinsics |= MONO_CFG_USES_SIMD_INTRINSICS;
		cfg->uses_simd_intrinsics |= MONO_CFG_USES_SIMD_INTRINSICS_DECOMPOSE_VTYPE;
	}
	return simd_inst;
}
#else
static
MonoInst*
emit_simd_intrinsics (const char *class_ns, const char *class_name, MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	return NULL;
}
#endif

MonoInst*
mono_emit_simd_intrinsics (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	const char *class_name;
	const char *class_ns;
	MonoImage *image = m_class_get_image (cmethod->klass);

	if (image != mono_get_corlib ())
		return NULL;

	class_ns = m_class_get_name_space (cmethod->klass);
	class_name = m_class_get_name (cmethod->klass);

	// If cmethod->klass is nested, the namespace is on the enclosing class.
	if (m_class_get_nested_in (cmethod->klass))
		class_ns = m_class_get_name_space (m_class_get_nested_in (cmethod->klass));

#if defined(TARGET_AMD64) || defined(TARGET_ARM64)
	if (!strcmp (class_ns, "System.Runtime.Intrinsics")) {
		if (!strcmp (class_name, "Vector128") || !strcmp (class_name, "Vector64"))
			return emit_sri_vector (cfg, cmethod, fsig, args);
	}
#endif // defined(TARGET_AMD64) || defined(TARGET_ARM64)

	return emit_simd_intrinsics (class_ns, class_name, cfg, cmethod, fsig, args);
}

/*
* Windows x64 value type ABI uses reg/stack references (ArgValuetypeAddrInIReg/ArgValuetypeAddrOnStack)
* for function arguments. When using SIMD intrinsics arguments optimized into OP_ARG needs to be decomposed
* into correspondig SIMD LOADX/STOREX instructions.
*/
#if defined(TARGET_WIN32) && defined(TARGET_AMD64)
static gboolean
decompose_vtype_opt_uses_simd_intrinsics (MonoCompile *cfg, MonoInst *ins)
{
	if (cfg->uses_simd_intrinsics & MONO_CFG_USES_SIMD_INTRINSICS_DECOMPOSE_VTYPE)
		return TRUE;

	switch (ins->opcode) {
	case OP_XMOVE:
	case OP_XZERO:
	case OP_LOADX_MEMBASE:
	case OP_LOADX_ALIGNED_MEMBASE:
	case OP_STOREX_MEMBASE:
	case OP_STOREX_ALIGNED_MEMBASE_REG:
		return TRUE;
	default:
		return FALSE;
	}
}

static void
decompose_vtype_opt_load_arg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, gint32 *sreg_int32)
{
	guint32 *sreg = (guint32*)sreg_int32;
	MonoInst *src_var = get_vreg_to_inst (cfg, *sreg);
	if (src_var && src_var->opcode == OP_ARG && src_var->klass && MONO_CLASS_IS_SIMD (cfg, src_var->klass)) {
		MonoInst *varload_ins, *load_ins;
		NEW_VARLOADA (cfg, varload_ins, src_var, src_var->inst_vtype);
		mono_bblock_insert_before_ins (bb, ins, varload_ins);
		MONO_INST_NEW (cfg, load_ins, OP_LOADX_MEMBASE);
		load_ins->klass = src_var->klass;
		load_ins->type = STACK_VTYPE;
		load_ins->sreg1 = varload_ins->dreg;
		load_ins->dreg = alloc_xreg (cfg);
		mono_bblock_insert_after_ins (bb, varload_ins, load_ins);
		*sreg = load_ins->dreg;
	}
}

void
mono_simd_decompose_intrinsic (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins)
{
	if (cfg->opt & MONO_OPT_SIMD && decompose_vtype_opt_uses_simd_intrinsics (cfg, ins)) {
		decompose_vtype_opt_load_arg (cfg, bb, ins, &(ins->sreg1));
		decompose_vtype_opt_load_arg (cfg, bb, ins, &(ins->sreg2));
		decompose_vtype_opt_load_arg (cfg, bb, ins, &(ins->sreg3));
		MonoInst *dest_var = get_vreg_to_inst (cfg, ins->dreg);
		if (dest_var && dest_var->opcode == OP_ARG && dest_var->klass && MONO_CLASS_IS_SIMD (cfg, dest_var->klass)) {
			MonoInst *varload_ins, *store_ins;
			ins->dreg = alloc_xreg (cfg);
			NEW_VARLOADA (cfg, varload_ins, dest_var, dest_var->inst_vtype);
			mono_bblock_insert_after_ins (bb, ins, varload_ins);
			MONO_INST_NEW (cfg, store_ins, OP_STOREX_MEMBASE);
			store_ins->klass = dest_var->klass;
			store_ins->type = STACK_VTYPE;
			store_ins->sreg1 = ins->dreg;
			store_ins->dreg = varload_ins->dreg;
			mono_bblock_insert_after_ins (bb, varload_ins, store_ins);
		}
	}
}
#else
void
mono_simd_decompose_intrinsic (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins)
{
}
#endif /*defined(TARGET_WIN32) && defined(TARGET_AMD64)*/

void
mono_simd_simplify_indirection (MonoCompile *cfg)
{
}

#endif /* DISABLE_JIT */
#endif /* MONO_ARCH_SIMD_INTRINSICS */

#if defined(TARGET_AMD64)
void
ves_icall_System_Runtime_Intrinsics_X86_X86Base___cpuidex (int abcd[4], int function_id, int subfunction_id)
{
#ifndef MONO_CROSS_COMPILE
	mono_hwcap_x86_call_cpuidex (function_id, subfunction_id,
		&abcd [0], &abcd [1], &abcd [2], &abcd [3]);
#endif
}
#endif

MONO_EMPTY_SOURCE_FILE (simd_intrinsics_netcore);
