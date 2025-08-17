# 总的Makefile，用于编译所有RB锁/信号量模块

# 定义模块目录
RB_MUTEX_DIR = rb_mutex
RB_RWMUTEX_DIR = rb_rwmutex
RB_RWSEM_DIR = rb_rwsem

# 默认目标
all: rb-modules

# 编译RB锁/信号量模块
rb-modules:
	@echo "编译RB锁/信号量模块..."
	$(MAKE) -C $(RB_MUTEX_DIR)
	$(MAKE) -C $(RB_RWMUTEX_DIR)
	$(MAKE) -C $(RB_RWSEM_DIR)



# 清理所有模块
clean:
	@echo "清理所有模块..."
	$(MAKE) -C $(RB_MUTEX_DIR) clean
	$(MAKE) -C $(RB_RWMUTEX_DIR) clean
	$(MAKE) -C $(RB_RWSEM_DIR) clean

# 安装模块
install:
	@echo "安装模块..."
	sudo insmod $(RB_MUTEX_DIR)/rb_mutex.ko
	sudo insmod $(RB_RWMUTEX_DIR)/rb_rwmutex.ko
	sudo insmod $(RB_RWSEM_DIR)/rb_rwsem.ko

# 卸载模块
uninstall:
	@echo "卸载模块..."
	sudo rmmod rb_rwsem
	sudo rmmod rb_rwmutex
	sudo rmmod rb_mutex

.PHONY: all rb-modules clean install uninstall 