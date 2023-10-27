use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
};

use super::sha256_short;
use indexmap::{IndexMap, IndexSet};
use luisa_compute_ir_v2::*;
use std::fmt::Write;
pub(crate) struct TypeGenInner {
    cache: HashMap<*const Type, String>,
    struct_typedefs: String,
}

impl TypeGenInner {
    pub(crate) fn new() -> Self {
        Self {
            cache: HashMap::new(),
            struct_typedefs: String::new(),
        }
    }
    fn to_c_type_(&mut self, t: *const Type) -> String {
        if t.is_null() {
            return "void".to_string();
        }
        if call!(type_is_scalar, t) {
            if call!(type_is_bool, t) {
                return "bool".to_string();
            } else if call!(type_is_int16, t) {
                return "int16_t".to_string();
            } else if call!(type_is_int32, t) {
                return "int32_t".to_string();
            } else if call!(type_is_int64, t) {
                return "int64_t".to_string();
            } else if call!(type_is_uint16, t) {
                return "uint16_t".to_string();
            } else if call!(type_is_uint32, t) {
                return "uint32_t".to_string();
            } else if call!(type_is_uint64, t) {
                return "uint64_t".to_string();
            } else if call!(type_is_float16, t) {
                return "half".to_string();
            } else if call!(type_is_float32, t) {
                return "float".to_string();
            } else {
                panic!("unknown type {:?}", call!(type_description, t));
            }
        } else if call!(type_is_vector, t) {
            if call!(type_is_bool, t) {
                return format!("lc_bool{}", call!(type_dimension, t));
            } else if call!(type_is_int16, t) {
                return format!("lc_short{}", call!(type_dimension, t));
            } else if call!(type_is_int32, t) {
                return format!("lc_int{}", call!(type_dimension, t));
            } else if call!(type_is_int64, t) {
                return format!("lc_long{}", call!(type_dimension, t));
            } else if call!(type_is_uint16, t) {
                return format!("lc_ushort{}", call!(type_dimension, t));
            } else if call!(type_is_uint32, t) {
                return format!("lc_uint{}", call!(type_dimension, t));
            } else if call!(type_is_uint64, t) {
                return format!("lc_ulong{}", call!(type_dimension, t));
            } else if call!(type_is_float16, t) {
                return format!("lc_half{}", call!(type_dimension, t));
            } else if call!(type_is_float32, t) {
                return format!("lc_float{}", call!(type_dimension, t));
            } else {
                todo!()
            }
        } else if call!(type_is_matrix, t) {
            let dim = call!(type_dimension, t);
            return format!("lc_float{0}x{0}", dim);
        } else if call!(type_is_struct, t) {
            let fields: &[*const Type] = call!(type_members, t).into();
            let alignment = call!(type_alignment, t);
            let field_types: Vec<String> = fields.iter().map(|f| self.to_c_type(*f)).collect();
            let field_types_str = field_types.join(", ");
            let hash = sha256_short(&format!("{}_alignas({})", field_types_str, alignment));
            let hash = hash.replace("-", "x_");
            let name = format!("s_{}", hash);

            self.cache.insert(t.clone(), name.clone());
            let mut tmp = String::new();
            writeln!(tmp, "struct alignas({0}) {1} {{", alignment, name).unwrap();
            for (i, field) in fields.iter().enumerate() {
                let field_name = format!("f{}", i);
                let field_type = self.to_c_type(*field);
                writeln!(tmp, "    {} {};", field_type, field_name).unwrap();
            }
            writeln!(tmp, "    __device__ constexpr static auto one() {{").unwrap();
            writeln!(tmp, "        return {0} {{", name).unwrap();
            for (_, field) in fields.as_ref().iter().enumerate() {
                let field_type = self.to_c_type(*field);
                writeln!(tmp, "        lc_one<{}>(),", field_type).unwrap();
            }
            writeln!(tmp, "        }};").unwrap();
            writeln!(tmp, "    }}").unwrap();
            writeln!(tmp, "    __device__ constexpr static auto zero() {{").unwrap();
            writeln!(tmp, "        return {0} {{", name).unwrap();
            for (_, field) in fields.as_ref().iter().enumerate() {
                let field_type = self.to_c_type(*field);
                writeln!(tmp, "        lc_zero<{}>(),", field_type).unwrap();
            }
            writeln!(tmp, "        }};").unwrap();
            writeln!(tmp, "    }}").unwrap();

            writeln!(tmp, "}};").unwrap();
            writeln!(
                tmp,
                "__device__ inline void lc_accumulate_grad({0} *dst, {0} grad) noexcept {{",
                name
            )
            .unwrap();
            for (i, _t) in fields.iter().enumerate() {
                let field_name = format!("f{}", i);
                writeln!(
                    tmp,
                    "        lc_accumulate_grad(&dst->{}, grad.{});",
                    field_name, field_name
                )
                .unwrap();
            }
            writeln!(tmp, "    }}").unwrap();
            self.struct_typedefs.push_str(&tmp);
        }
        todo!()
    }
    pub(crate) fn to_c_type(&mut self, t: *const Type) -> String {
        if let Some(t) = self.cache.get(&t) {
            return t.clone();
        } else {
            let t_ = self.to_c_type_(t);
            self.cache.insert(t.clone(), t_.clone());
            return t_;
        }
    }
}

struct TypeGen {
    inner: RefCell<TypeGenInner>,
}

impl TypeGen {
    fn new() -> Self {
        Self {
            inner: RefCell::new(TypeGenInner::new()),
        }
    }
    fn gen_c_type(&self, t: *const Type) -> String {
        self.inner.borrow_mut().to_c_type(t)
    }
    fn generated(&self) -> String {
        self.inner.borrow().struct_typedefs.clone()
    }
}

struct GlobalEmitter {
    message: Vec<String>,
    global_vars: HashMap<NodeRef, String>,
    generated_callables: HashMap<u64, String>,
    generated_callable_sources: HashMap<String, String>,
    callable_def: String,
    captures: IndexMap<NodeRef, usize>,
    args: IndexMap<NodeRef, usize>,
    cpu_custom_ops: IndexMap<usize, usize>,
}

struct FunctionEmitter<'a> {
    type_gen: &'a TypeGen,
    node_to_var: HashMap<NodeRef, String>,
    body: String,
    fwd_defs: String,
    phis: IndexSet<NodeRef>,
    phis_per_block: IndexMap<*const BasicBlock, Vec<NodeRef>>,
    indent: usize,
    visited: HashSet<NodeRef>,
    globals: &'a mut GlobalEmitter,
    inside_generic_loop: bool,
    // message: Vec<String>,
}

pub struct PhiCollector {
    phis: IndexSet<NodeRef>,
    phis_per_block: IndexMap<*const BasicBlock, Vec<NodeRef>>,
}

impl PhiCollector {
    pub fn new() -> Self {
        Self {
            phis: IndexSet::new(),
            phis_per_block: IndexMap::new(),
        }
    }
    pub unsafe fn visit_block(&mut self, block: *const BasicBlock) {
        for phi in basic_block_phis(block) {
            self.phis.insert(phi);
            let phi_node = phi;
            if let Some(phi) =
                call!(Instruction_as_PhiInst, call!(node_inst, phi) as *mut _).as_ref()
            {
                let incomings: &[PhiIncoming] =
                    call!(PhiInst_incomings, phi as *const _ as *mut _).into();
                for incoming in incomings.as_ref() {
                    let ptr = incoming.block as *const BasicBlock;
                    self.phis_per_block
                        .entry(ptr)
                        .or_insert_with(Vec::new)
                        .push(phi_node);
                }
            } else {
                unreachable!()
            }
        }
        for node in basic_block_iter(block) {
            let inst = call!(node_inst, node) as *mut _;
            let tag = call!(Instruction_tag, inst);
            match tag {
                InstructionTag::IF => {
                    let if_ = call!(Instruction_as_IfInst, inst);
                    let true_branch = call!(IfInst_true_branch, if_);
                    let false_branch = call!(IfInst_false_branch, if_);
                    self.visit_block(true_branch);
                    self.visit_block(false_branch);
                }
                InstructionTag::SWITCH => {
                    let sw = call!(Instruction_as_SwitchInst, inst);
                    let default = call!(SwitchInst_default_, sw);
                    let cases: &[SwitchCase] = call!(SwitchInst_cases, sw).into();
                    self.visit_block(default);
                    for SwitchCase { value: _, block } in cases.as_ref() {
                        self.visit_block(*block);
                    }
                }
                InstructionTag::GENERIC_LOOP => {
                    let generic_loop = call!(Instruction_as_GenericLoopInst, inst);
                    let prepare = call!(GenericLoopInst_prepare, generic_loop);
                    let body = call!(GenericLoopInst_body, generic_loop);
                    let update = call!(GenericLoopInst_update, generic_loop);
                    self.visit_block(prepare);
                    self.visit_block(body);
                    self.visit_block(update);
                }
                InstructionTag::RAY_QUERY => {
                    let rq = call!(Instruction_as_RayQueryInst, inst);
                    let on_triangle_hit = call!(RayQueryInst_on_triangle_hit, rq);
                    let on_procedural_hit = call!(RayQueryInst_on_procedural_hit, rq);
                    self.visit_block(on_triangle_hit);
                    self.visit_block(on_procedural_hit);
                }
                InstructionTag::FWD_AUTODIFF => {
                    let fwd = call!(Instruction_as_FwdAutodiffInst, inst);
                    let body = call!(FwdAutodiffInst_body, fwd);
                    self.visit_block(body);
                }
                InstructionTag::REV_AUTODIFF => {
                    let rev = call!(Instruction_as_RevAutodiffInst, inst);
                    let body = call!(RevAutodiffInst_body, rev);
                    self.visit_block(body);
                }
                _ => {}
            }
        }
    }
}

impl<'a> FunctionEmitter<'a> {
    fn new(globals: &'a mut GlobalEmitter, type_gen: &'a TypeGen) -> Self {
        Self {
            type_gen,
            node_to_var: HashMap::new(),
            body: String::new(),
            fwd_defs: String::new(),
            phis: IndexSet::new(),
            phis_per_block: IndexMap::new(),
            indent: 1,
            visited: HashSet::new(),
            globals,
            inside_generic_loop: false,
        }
    }
    fn write_ident(&mut self) {
        for _ in 0..self.indent {
            write!(&mut self.body, "    ").unwrap();
        }
    }
    fn gen_node(&mut self, node: NodeRef) -> String {
        if let Some(var) = self.node_to_var.get(&node) {
            return var.clone();
        } else {
            let index = self.node_to_var.len();
            let inst = call!(node_inst, node) as *mut CInstruction;
            let var = match call!(Instruction_tag, inst) {
                InstructionTag::BUFFER => format!("b{}", index),
                InstructionTag::BINDLESS_ARRAY => format!("bl{}", index),
                InstructionTag::TEXTURE2D => format!("t2d{}", index),
                InstructionTag::TEXTURE3D => format!("t3d{}", index),
                InstructionTag::ACCEL => format!("a{}", index),
                InstructionTag::SHARED => format!("s{}", index),
                InstructionTag::UNIFORM => format!("u{}", index),
                InstructionTag::LOCAL => format!("v{}", index),
                InstructionTag::ARGUMENT => format!("arg{}", index),
                InstructionTag::CONSTANT => format!("c{}", index),
                InstructionTag::CALL => format!("f{}", index),
                InstructionTag::PHI => format!("phi{}", index),
                _ => unreachable!(),
            };
            self.node_to_var.insert(node, var.clone());
            return var;
        }
    }
    fn access_chain(&mut self, mut var: String, node: NodeRef, indices: &[NodeRef]) -> String {
        let mut ty = call!(node_type, node);
        for (i, index) in indices.iter().enumerate() {
            if call!(type_is_matrix, ty) || call!(type_is_vector, ty) {
                var = format!("{}[{}]", var, self.gen_node(*index));
                assert_eq!(i, indices.len() - 1);
                break;
            } else if call!(type_is_array, ty) {
                var = format!("{}[{}]", var, self.gen_node(*index));
                ty = call!(type_element, ty);
            } else {
                assert!(call!(type_is_struct, ty));
                let idx = call!(node_get_index, *index) as u32;
                var = format!("{}.f{}", var, idx);
                ty = call!(type_extract, ty, idx);
            }
        }
        var
    }
    fn atomic_chain_op(
        &mut self,
        var: &str,
        node_ty_s: &String,
        args: &[NodeRef],
        args_v: &[String],
        op: &str,
        noperand: usize,
    ) {
        let n = args.len();
        let buffer_ty = self.type_gen.gen_c_type(call!(node_type, args[0]));
        let indices = &args[2..n - noperand];
        let buffer_ref = format!(
            "(*lc_buffer_ref<{0}>(k_args, {1}, {2}))",
            buffer_ty, args_v[0], args_v[1]
        );
        let access_chain = self.access_chain(buffer_ref, args[0], indices);
        writeln!(
            self.body,
            "const {} {} = {}(&{}, {});",
            node_ty_s,
            var,
            op,
            access_chain,
            args_v[n - noperand..].join(", ")
        )
        .unwrap();
    }
    fn gep_field_name(node: NodeRef, i: i32) -> String {
        let ty = call!(node_type, node);
        if call!(type_is_struct, ty) {
            format!("f{}", i)
        } else if call!(type_is_vector, ty) {
            match i {
                0 => "x".to_string(),
                1 => "y".to_string(),
                2 => "z".to_string(),
                3 => "w".to_string(),
                _ => unreachable!(),
            }
        } else if call!(type_is_matrix, ty) {
            format!("cols[{}]", i)
        } else {
            unreachable!()
        }
    }
}
