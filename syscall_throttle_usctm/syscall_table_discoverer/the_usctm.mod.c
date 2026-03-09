#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x0040afbe, "param_ops_ulong" },
	{ 0xd272d446, "__SCT__preempt_schedule" },
	{ 0x0040afbe, "param_array_ops" },
	{ 0xd272d446, "__fentry__" },
	{ 0x5a844b26, "__x86_indirect_thunk_rax" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xbd03ed67, "page_offset_base" },
	{ 0x2719b9fa, "const_current_task" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x6f8082dd, "pv_ops" },
	{ 0x7ec472ba, "__preempt_count" },
	{ 0x1c489eb6, "register_kprobe" },
	{ 0x7a8e92c6, "unregister_kprobe" },
	{ 0x0040afbe, "param_ops_int" },
	{ 0xd272d446, "BUG_func" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x0040afbe,
	0xd272d446,
	0x0040afbe,
	0xd272d446,
	0x5a844b26,
	0xe8213e80,
	0xbd03ed67,
	0xd272d446,
	0x90a48d82,
	0xbd03ed67,
	0x2719b9fa,
	0xd272d446,
	0x6f8082dd,
	0x7ec472ba,
	0x1c489eb6,
	0x7a8e92c6,
	0x0040afbe,
	0xd272d446,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"param_ops_ulong\0"
	"__SCT__preempt_schedule\0"
	"param_array_ops\0"
	"__fentry__\0"
	"__x86_indirect_thunk_rax\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"__stack_chk_fail\0"
	"__ubsan_handle_out_of_bounds\0"
	"page_offset_base\0"
	"const_current_task\0"
	"__x86_return_thunk\0"
	"pv_ops\0"
	"__preempt_count\0"
	"register_kprobe\0"
	"unregister_kprobe\0"
	"param_ops_int\0"
	"BUG_func\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "8435A1FD2AB82DC917C98EB");
