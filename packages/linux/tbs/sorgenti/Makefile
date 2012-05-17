#dvb-usb-objs = dvb-usb-firmware.o dvb-usb-init.o dvb-usb-urb.o dvb-usb-i2c.o dvb-usb-dvb.o dvb-usb-remote.o usb-urb.o
#obj-$(CONFIG_DVB_USB) += dvb-usb.o

dvb-usb-tbsqbox-objs = tbs-qbox.o
obj-$(CONFIG_DVB_USB_TBSQBOX) += dvb-usb-tbsqbox.o

dvb-usb-tbsqbox2-objs = tbs-qbox2.o
obj-$(CONFIG_DVB_USB_TBSQBOX2) += dvb-usb-tbsqbox2.o

dvb-usb-tbsqbox2ci-objs = tbs-qbox2ci.o
obj-$(CONFIG_DVB_USB_TBSQBOX2CI) += dvb-usb-tbsqbox2ci.o

dvb-usb-tbsqboxs2-objs = tbs-qboxs2.o
obj-$(CONFIG_DVB_USB_TBSQBOXS2) += dvb-usb-tbsqboxs2.o

dvb-usb-tbsqboxs3-objs = tbs-qboxs3.o tbs5921ctrl.o
obj-$(CONFIG_DVB_USB_TBSQBOXS3) += dvb-usb-tbsqboxs3.o

dvb-usb-tbsqbox22-objs = tbs-qbox22.o tbs5922ctrl.o
obj-$(CONFIG_DVB_USB_TBSQBOX22) += dvb-usb-tbsqbox22.o

dvb-usb-tbs5880-objs = tbs5880.o tbs5880ctrl.o
obj-$(CONFIG_DVB_USB_TBS5880) += dvb-usb-tbs5880.o

EXTRA_CFLAGS += -Idrivers/media/dvb/dvb-core/ -Idrivers/media/dvb/frontends/
# due to tuner-xc3028
EXTRA_CFLAGS += -Idrivers/media/common/tuners

