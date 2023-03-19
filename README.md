uinth
-----

uinth is a simple (demo) application that handles interrupts in userspace using
the UIO interface of the Linux kernel. For interrupts coming from GPIOs, the
current GPIO value can be queried, too.

To configure a UIO interface, some changes in the device tree are necessary.

As example, the following entries could be used:

```
/ {
	[...]
	
	uio_my_gpio@0 {
		compatible = "generic-uio,ui_pdrv";
		status = "okay";
		interrupt-parent = <&gpio0>;
		interrupts = <10 0>;
		gpiopath = &my_gpio;
	};
	
	uio_my_regular_interrupt@0 {
		compatible = "generic-uio,ui_pdrv";
		status = "okay";
		interrupt-parent = <&gic>;
		interrupts = <GIC_SPI 123 IRQ_TYPE_EDGE_RISING>;
	};
};

&gpio0 {
	my_gpio: my_gpio {
		gpios = <10 0>;
		input;
	};
};
```

The `gpiopath` property in `uio_my_gpio@0` is a custom link for uinth to find
the corresponding GPIO entry as it is not easy to find a single GPIO by name.

The `compatible` string in the device tree can be chosen freely. However, it has
to match the `of_id` parameter of the `uio_pdrv_genirq` module. For the example
above, the corresponding kernel parameter would look like:

```
uio_pdrv_genirq.of_id="generic-uio,ui_pdrv"
```

The corresponding `uinth.cfg` could look like this:

```
[uio_my_gpio]
# instruct uinth to also get the GPIO value
flags = 1
cmd = echo Interrupt my_gpio with value %d

[uio_my_regular_interrupt]
cmd = echo my_regular_interrupt triggered
```
