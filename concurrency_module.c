#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/spinlock_types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/moduleparam.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Danyil Peschanskyi");
MODULE_VERSION("0.1");

#define GPIO_NUMBER(port, bit) (32 * (port) + (bit))

#define LED_SD  GPIO_NUMBER(1, 22)
#define LED_MMC GPIO_NUMBER(1, 24)
#define BUTTON  GPIO_NUMBER(2, 8)

#define NUM 5

#define LIST_FREE(list_data_type, list_head, member) \
	do {									\
		list_data_type *__ptr, *__tmp;					\
		list_for_each_entry_safe(__ptr, __tmp, (list_head), member) {	\
			kfree(__ptr);						\
		}								\
	} while(0)

#define IS_NULL(list_data_ptr) \
	((list_data_ptr)->number == 0 && (list_data_ptr)->time == 0)

typedef struct list_data {
	struct list_head list_node;
	int number;
	ktime_t time;
} list_data_t;

typedef struct locked_list_head {
	spinlock_t spinlock;
	struct list_head list_head;
	int cur_num;
} locked_list_head_t;

static locked_list_head_t locked_list_head = {
	.spinlock = __SPIN_LOCK_UNLOCKED(spinlock),
	.list_head = LIST_HEAD_INIT(locked_list_head.list_head),
	.cur_num = 0
};

static struct timer_list timer;
static unsigned long start;
static unsigned long delay_in_jiffies = HZ * 1L;
static bool restart = true;

static int button_gpio = -1;

static irqreturn_t button_thread(int irq, void *dev_id)
{
	int num = 0;
	ktime_t time;
	list_data_t *ptr;
	spin_lock(&locked_list_head.spinlock);
	list_for_each_entry(ptr, &locked_list_head.list_head, list_node)
	{
		if (!IS_NULL(ptr)) {
			num = ptr->number;
			ptr->number = 0;
			time = ptr->time;
			ptr->time = 0;
			break;
		}
	}
	spin_unlock(&locked_list_head.spinlock);
	if (num)
		pr_info("Element with num: %i, time: %lld\n", num, time);
	else
		pr_info("Filled element does not exist\n");
	return IRQ_HANDLED;
}

static int button_gpio_init(int gpio)
{
	int rc;

	rc = gpio_request(gpio, "Onboard user button");
	if (rc)
		goto err_register;

	rc = gpio_direction_input(gpio);
	if (rc)
		goto err_input;

	button_gpio = gpio;
	pr_info("Init GPIO%d OK\n", button_gpio);
	return 0;

err_input:
	gpio_free(gpio);
err_register:
	return rc;
}

static void button_gpio_deinit(void)
{
	if (button_gpio >= 0) {
		gpio_free(button_gpio);
		pr_info("Deinit GPIO%d\n", button_gpio);
	}
}

static void timer_callback(struct timer_list *timer)
{
	unsigned long flags;
	ktime_t cur_time;
	list_data_t *ptr;
	pr_info("Time callback starts\n");
	++locked_list_head.cur_num;
	cur_time = ktime_get();

	spin_lock_irqsave(&locked_list_head.spinlock, flags);
	ptr = list_first_entry(&locked_list_head.list_head,
			       list_data_t, list_node);
	if (IS_NULL(ptr)) {
		pr_info("Initializing ptr\n");
		ptr->number = locked_list_head.cur_num;
		ptr->time = cur_time;
		list_rotate_left(&locked_list_head.list_head);
		pr_info("ptr initialized\n");
	}
	else {
		pr_info("list overflow with num: %i\n", locked_list_head.cur_num);
	}
	spin_unlock_irqrestore(&locked_list_head.spinlock, flags);
	if (restart) {
		start += delay_in_jiffies;
		mod_timer(timer, start);
	}
}

static int __init concurrency_module_init(void)
{
	int i, rc, button_irq;
	unsigned long now;

	pr_info("Initializing list\n");
	for (i = 0; i < NUM; ++i) {
		list_data_t *ptr = (list_data_t *)kmalloc(sizeof(*ptr),
							  GFP_KERNEL);
		if (!ptr) {
			pr_err("Can't alloc list data\n");
			rc = -1;
			goto err_list;
		}
		ptr->time = 0;
		ptr->number = 0;
		list_add_tail(&ptr->list_node, &locked_list_head.list_head);
	}
	pr_info("List Initialized\n");

	pr_info("Initializing timer\n");
        timer_setup(&timer, timer_callback, 0);

        now = jiffies;
        start = now + delay_in_jiffies;
        pr_info("Starting timer to fire in %lu ms (%lu)\n", start, now);

        mod_timer(&timer, start);
	pr_info("Timer Initialized\n");

	rc = button_gpio_init(BUTTON);
	if (rc) {
		pr_err("Can't set GPIO%d for button\n", BUTTON);
		goto err_button;
	}

	button_irq = gpio_to_irq(button_gpio);
	rc = request_threaded_irq(button_irq, NULL, button_thread,
	                          IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				  "test", &locked_list_head);

	if (rc) {
		pr_err("Can't set threaded irq\n");
		goto err_irq;
	}

	return 0;
err_irq:
	free_irq(button_irq, &locked_list_head);
err_button:
err_list:
	LIST_FREE(list_data_t, &locked_list_head.list_head, list_node);
	return rc;
}

static void __exit concurrency_module_exit(void)
{
	button_gpio_deinit();
	del_timer(&timer);
	LIST_FREE(list_data_t, &locked_list_head.list_head, list_node);
}

module_init(concurrency_module_init);
module_exit(concurrency_module_exit);
