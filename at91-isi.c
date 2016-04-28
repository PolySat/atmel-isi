/*
 * Copyright (c) 2007 Atmel Corporation
 *
 * Based on the bttv driver for Bt848 with respective copyright holders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef DEBUG
#define DEBUG 1
#endif

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/bootmem.h>

#include <linux/kfifo.h>

#include <asm/io.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>

#include <media/at91-isi.h>

#define AT91_ISI_VERSION	KERNEL_VERSION(0, 1, 0)
#define ISI_CODEC 0

/* Default ISI capture buffer size */
#define ISI_CAPTURE_BUFFER_SIZE 1600*1200*2
/* Default ISI video frame size */
#define ISI_VIDEO_BUFFER_SIZE 320*240*2
/* Default number of ISI video buffers */
#define ISI_VIDEO_BUFFERS 1
/* Maximum number of video buffers */
#define ISI_VIDEO_BUFFERS_MAX 8

/* Interrupt mask for a single capture */
#define ISI_CAPTURE_MASK (ISI_BIT(SOF) | ISI_BIT(FO_C_EMP))

/* ISI capture buffer size */
static int capture_buffer_size = ISI_CAPTURE_BUFFER_SIZE;
/* Number of buffers used for streaming video */
static int video_buffers = ISI_VIDEO_BUFFERS;
static int video_buffer_size = ISI_VIDEO_BUFFER_SIZE;

static int input_format = AT91_ISI_PIXFMT_YCbYCr;
static u8 has_emb_sync = 0;
static u8 emb_crc_sync = 0;
static u8 hsync_act_low = 0;
static u8 vsync_act_low = 1;
//pclk here refers to the pixel clock coming from a sensor
static u8 pclk_act_falling = 0;

static u8 isi_full_mode = 0;
static u8  gs_mode = 0;
/* Preview path horizontal size */
static int prev_hsize = 320;
/* Preview path vertical size */
static int prev_vsize = 240;
/* Scaling factor of the preview path */
static int prev_decimation_factor = 16;

/* Input image horizontal size */
static int image_hsize = 1600;

/* Input image vertical size */
static int image_vsize = 1200; 

/* Frame rate scaler 
 * 1 = capture every second frame
 * 2 = capture every third frame
 * ...
 * */
static int frame_rate_scaler = 2;

/* Set this value if we want to pretend a specific V4L2 output format 
 *  This format is for the capturing interface
 */ 
static int capture_v4l2_fmt = V4L2_PIX_FMT_VYUY;

/* Set this value if we want to pretend a specific V4L2 output format 
 *  This format is for the streaming interface
 */
static int streaming_v4l2_fmt = V4L2_PIX_FMT_VYUY;

MODULE_PARM_DESC(video_buffers,"Number of frame buffers used for streaming");
module_param(video_buffers, int, 0664);
MODULE_PARM_DESC(capture_buffer_size,"Capture buffer size");
module_param(capture_buffer_size, int, 0664);
MODULE_PARM_DESC(image_hsize,"Horizontal size of input image");
module_param(image_hsize, int, 0664);
MODULE_PARM_DESC(image_vsize,"Vertical size of input image");
module_param(image_vsize, int, 0664);
MODULE_PARM_DESC(frame_rate_scaler, "Frame rate scaler");
module_param(frame_rate_scaler, int, 0664);
MODULE_PARM_DESC(prev_hsize, "Horizontal image size of preview path output");
module_param(prev_hsize, int, 0664);
MODULE_PARM_DESC(prev_vsize, "Vertical image size of preview path output");
module_param(prev_vsize, int, 0664);
MODULE_PARM_DESC(prev_decimation_factor, "Preview path decimaion factor");
module_param(prev_decimation_factor, int, 0664);
/* Single frame capturing states */
enum {
	STATE_IDLE = 0,
	STATE_CAPTURE_READY,
	STATE_CAPTURE_WAIT_SOF,
	STATE_CAPTURE_IN_PROGRESS,
	STATE_CAPTURE_DONE,
	STATE_CAPTURE_ERROR,
};

/* Frame buffer states
 *  FRAME_UNUSED Frame(buffer) is not used by the ISI module -> an application
 *  can usually read out data in this state
 *  FRAME_QUEUED An application has queued the buffer in the incoming queue 
 *  FRAME_DONE The ISI module has filled the buffer with data and placed is on
 *  the outgoing queue
 *  FRAME_ERROR Not used at the moment
 *  */
enum frame_status {
	FRAME_UNUSED,
	FRAME_QUEUED,
	FRAME_DONE,
	FRAME_ERROR,
};
/* Frame buffer descriptor 
 *  Used by the ISI module as a linked list for the DMA controller.
 */
struct fbd {
	/* Physical address of the frame buffer */
	dma_addr_t fb_address;
	/* Physical address of the next fbd */
	dma_addr_t next_fbd_address;
};

/* Frame buffer data
 */
struct frame_buffer {
	/*  Frame buffer descriptor
	 *  Used by the ISI DMA controller to provide linked list DMA operation
	 */
	struct fbd fb_desc;
	/* Pointer to the start of the frame buffer */
	void *frame_buffer;
	/* Timestamp of the captured frame */
	struct timeval timestamp;
	/* Frame number of the frame  */
	unsigned long sequence;
	/* Buffer number*/
	int index;
	/* Bytes used in the buffer for data, needed as buffers are always
	 *  aligned to pages and thus may be bigger than the amount of data*/
	int bytes_used;
	/* Mmap count
	 *  Counter to measure how often this buffer is mmapped 
	 */
	int mmap_count;
	/* Buffer status */
	enum frame_status status;
};

struct at91_isi {
	/* ISI module spin lock. Protects against concurrent access of variables
	 * that are shared with the ISR */
	spinlock_t			lock;
	void __iomem			*regs;
	/* Pointer to the start of the fbd list */
	dma_addr_t			fbd_list_start;
	/* Frame buffers */
	struct frame_buffer 		video_buffer[ISI_VIDEO_BUFFERS_MAX];
	/* Frame buffer currently used by the ISI module */
	struct frame_buffer		*current_buffer;
	/* Size of a frame buffer */
	size_t				capture_buffer_size;
	/* Streaming status 
	 *  If set ISI is in streaming mode */
	int				streaming;
	/* Queue for incoming buffers
	 *  The buffer number (index) is stored in the fifo as reference 
	 */
	struct kfifo 			*grabq;
	/* Spinlock for the incoming queue */
	spinlock_t 			grabq_lock;
	/* Queue for outgoing buffers
	 *  Buffer number is stored in the fifo as reference
	 */
	struct kfifo			*doneq;
	/* Spinlock for the incoming queue */
	spinlock_t			doneq_lock;

	/* State of the ISI module in capturing mode */
	int				state;
	/* Pointer to ISI buffer */
	void				*capture_buf;
	/* Physical address of the capture buffer */
	dma_addr_t			capture_phys;
	/* Size of the ISI buffer */
	size_t				capture_buf_size;
	/* Capture/streaming wait queue */
	wait_queue_head_t		capture_wq;

	struct at91_isi_camera		*camera;
	struct at91_isi_format		format;
	struct at91_isi_format		streaming_format;

	struct mutex			mutex;
	/* User counter for the streaming interface */
	int				stream_users;
	/* User counter of the capture interface */
	int				capture_users;
	/* Video device for capturing (Codec path) */
	struct video_device		cdev;

	/* Video device for streaming (Preview path) */
	struct video_device		vdev;
	struct completion		reset_complete;
	struct clk			*pclk;
	struct clk			*hclk;
	struct platform_device		*pdev;
	unsigned int			irq;
};

#define to_at91_isi(vdev) container_of(vdev, struct at91_isi, vdev)

struct at91_isi_fh {
	struct at91_isi			*isi;
	unsigned int			read_off;
};

/*-----------------------------------------------------------------------------
 * Interface to the actual camera.
 */
static LIST_HEAD(camera_list);
static DEFINE_MUTEX(camera_list_mutex);

static void at91_isi_release_camera(struct at91_isi *isi, struct at91_isi_camera *cam)
{
	mutex_lock(&camera_list_mutex);
	cam->isi = NULL;
	isi->camera = NULL;
	module_put(cam->owner);
	mutex_unlock(&camera_list_mutex);
}

int at91_isi_register_camera(struct at91_isi_camera *cam)
{
	pr_debug("at91_isi: register camera %s\n", cam->name);

	mutex_lock(&camera_list_mutex);
	list_add_tail(&cam->list, &camera_list);
	mutex_unlock(&camera_list_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(at91_isi_register_camera);

void at91_isi_unregister_camera(struct at91_isi_camera *cam)
{
	pr_debug("at91_isi: unregister camera %s\n", cam->name);
	mutex_lock(&camera_list_mutex);
	if (cam->isi)
		cam->isi->camera = NULL;
	list_del(&cam->list);
	mutex_unlock(&camera_list_mutex);
}
EXPORT_SYMBOL_GPL(at91_isi_unregister_camera);

static struct at91_isi_camera * at91_isi_grab_camera(struct at91_isi *isi)
{
	struct at91_isi_camera *entry, *cam = NULL;
	mutex_lock(&camera_list_mutex);
	list_for_each_entry(entry, &camera_list, list) 
	{
		/* Just grab the first camera available */
		if (!entry->isi) 
		{
			if (!try_module_get(entry->owner))
				continue;

			cam = entry;
			cam->isi = isi;
			pr_debug("%s: got camera: %s\n",
				 isi->vdev.name, cam->name);
			break;
		}
	}
	mutex_unlock(&camera_list_mutex);

	return cam;
}

static int at91_isi_set_camera_input(struct at91_isi *isi)
{
	struct at91_isi_camera *cam = isi->camera;
	int ret;
	ret = cam->set_format(cam, &isi->format);
	if (ret)
		return ret;
	return 0;
}
static void at91_isi_set_default_format(struct at91_isi *isi)
{

	isi->format.pix.width = min((int)2048l, image_hsize);
	isi->format.pix.height = min((int)2048l, image_vsize);

	/* Set capture format if we have explicitely specified one */
	if(capture_v4l2_fmt){
		isi->format.pix.pixelformat = capture_v4l2_fmt;
	}
	else {
		/* Codec path output format */
		isi->format.pix.pixelformat = V4L2_PIX_FMT_VYUY;
	}

	/* The ISI module codec path tries to output YUV 4:2:2
	 * Therefore two pixels will be in a 32bit word */
	isi->format.pix.bytesperline = ALIGN(isi->format.pix.width * 2, 4);
	isi->format.pix.sizeimage = isi->format.pix.bytesperline * 
		isi->format.pix.height;

	pr_debug("set capture format: width=%d height=%d\n",
		isi->format.pix.width, isi->format.pix.height);


#ifdef ISI_CODEC
	isi->streaming_format.pix.width = isi->format.pix.width;
	isi->streaming_format.pix.height = isi->format.pix.height;
	isi->streaming_format.pix.bytesperline = isi->format.pix.bytesperline;
	isi->streaming_format.pix.sizeimage = isi->format.pix.sizeimage;
#else
	isi->streaming_format.pix.width = min(320U, prev_hsize);
	isi->streaming_format.pix.height = min(240U, prev_vsize);

	/* The ISI module preview path outputs either RGB 5:5:5
	 * or grayscale mode. Normally 2 pixels are stored in one word. 
	 * But since the grayscale mode offers the possibility to store 1 pixel
	 * in one word we have to adjust the size here.
	 */
	if(input_format == AT91_ISI_PXFMT_GREY 
		&& gs_mode == ISI_GS_1PIX_PER_WORD) {

		isi->streaming_format.pix.bytesperline =
			ALIGN(isi->streaming_format.pix.width *4, 4);
	}
	else {
		isi->streaming_format.pix.bytesperline = 
			ALIGN(isi->streaming_format.pix.width * 2, 4);
	}

	isi->streaming_format.pix.sizeimage = 
		isi->streaming_format.pix.bytesperline * 
		isi->streaming_format.pix.height;
#endif
	/* Set streaming format if we have explicitely specified one */
	if(streaming_v4l2_fmt){
		isi->streaming_format.pix.pixelformat = streaming_v4l2_fmt;
	}
	else {
		/* Preview path output format
		 * Would be logically V4L2_PIX_FMT_BGR555X 
		 * but this format does not exist in the specification
		 * So for now we pretend V4L2_PIX_FMT_RGB555X 
		 * Also the Greyscale format does not fit on top of the V4L2
		 * format but for now we just return it.
		 */
		if(input_format == AT91_ISI_PIXFMT_GREY)
			isi->streaming_format.pix.pixelformat = V4L2_PIX_FMT_GREY;
		else
			isi->streaming_format.pix.pixelformat = V4L2_PIX_FMT_RGB555X;
	}
	
	if(input_format){
		isi->format.input_format = input_format;
		/* Not needed but for completeness*/
		isi->streaming_format.input_format = input_format;
	}

}
static int at91_isi_init_hardware(struct at91_isi *isi)
{
	u32 cr2, cr1, cr;
	
	cr = 0;
	switch (isi->format.input_format) {
	case AT91_ISI_PIXFMT_GREY:
		cr = ISI_BIT(GRAYSCALE);
		break;
	case AT91_ISI_PIXFMT_CbYCrY:
		cr = ISI_BF(YCC_SWAP, 0);
		break;
	case AT91_ISI_PIXFMT_CrYCbY:
		cr = ISI_BF(YCC_SWAP, 1);
		break;
	case AT91_ISI_PIXFMT_YCbYCr:
		cr = ISI_BF(YCC_SWAP, 2);
		break;
	case AT91_ISI_PIXFMT_YCrYCb:
		cr = ISI_BF(YCC_SWAP, 3);
		break;
	case AT91_ISI_PIXFMT_RGB24:
		cr = ISI_BIT(COL_SPACE) | ISI_BF(RGB_CFG, 0);
		break;
	case AT91_ISI_PIXFMT_BGR24:
		cr = ISI_BIT(COL_SPACE) | ISI_BF(RGB_CFG, 1);
		break;
	case AT91_ISI_PIXFMT_RGB16:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_MODE)
		       | ISI_BF(RGB_CFG, 0));
		break;
	case AT91_ISI_PIXFMT_BGR16:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_MODE)
		       | ISI_BF(RGB_CFG, 1));
		break;
	case AT91_ISI_PIXFMT_GRB16:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_MODE)
		       | ISI_BF(RGB_CFG, 2));
		break;
	case AT91_ISI_PIXFMT_GBR16:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_MODE)
		       | ISI_BF(RGB_CFG, 3));
		break;
	case AT91_ISI_PIXFMT_RGB24_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BF(RGB_CFG, 0));
		break;
	case AT91_ISI_PIXFMT_BGR24_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BF(RGB_CFG, 1));
		break;
	case AT91_ISI_PIXFMT_RGB16_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BIT(RGB_MODE) | ISI_BF(RGB_CFG, 0));
		break;
	case AT91_ISI_PIXFMT_BGR16_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BIT(RGB_MODE) | ISI_BF(RGB_CFG, 1));
		break;
	case AT91_ISI_PIXFMT_GRB16_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BIT(RGB_MODE) | ISI_BF(RGB_CFG, 2));
		break;
	case AT91_ISI_PIXFMT_GBR16_REV:
		cr = (ISI_BIT(COL_SPACE) | ISI_BIT(RGB_SWAP)
		       | ISI_BIT(RGB_MODE) | ISI_BF(RGB_CFG, 3));
		break;
	default:
		return -EINVAL;
	}

	
	cr1 = ISI_BF(EMB_SYNC, has_emb_sync)
		| ISI_BF(HSYNC_POL, hsync_act_low)
		| ISI_BF(VSYNC_POL, vsync_act_low)
		| ISI_BF(PIXCLK_POL, pclk_act_falling)
		| ISI_BF(GS_MODE, gs_mode)
		| ISI_BF(FULL, isi_full_mode)
		| ISI_BIT(DIS);
	isi_writel(isi, CR1, cr1);

#ifndef ISI_CODEC
	/* These values depend on the sensor output image size */
	isi_writel(isi, PDECF, prev_decimation_factor);/* 1/16 * 16 = 1*/
	isi_writel(isi,PSIZE , ISI_BF(PREV_HSIZE,prev_hsize - 1)
		| ISI_BF(PREV_VSIZE, prev_vsize - 1));
#endif
	cr2 = isi_readl(isi, CR2);
	cr2 |= cr;
	cr2 = ISI_BFINS(IM_VSIZE, isi->format.pix.height - 1, cr2);
	cr2 = ISI_BFINS(IM_HSIZE, isi->format.pix.width - 1, cr2);
	isi_writel(isi, CR2, cr2);

	pr_debug("set_format: cr1=0x%08x\n cr2=0x%08x\n",
		 isi_readl(isi, CR1), isi_readl(isi, CR2));
	pr_debug("psize=0x%08x\n", isi_readl(isi, PSIZE));
	return 0;
}

static int at91_isi_start_capture(struct at91_isi *isi)
{
	u32 cr1;
	int ret;

	spin_lock_irq(&isi->lock);
	isi->state = STATE_IDLE;
	isi_readl(isi, SR); /* clear any pending SOF interrupt */
	isi_writel(isi, IER, ISI_BIT(SOF));
	isi_writel(isi, CR1, isi_readl(isi, CR1) & ~ISI_BIT(DIS));	
	spin_unlock_irq(&isi->lock);

	pr_debug("isi: waiting for SOF\n");
	ret = wait_event_interruptible(isi->capture_wq,
				       isi->state != STATE_IDLE);
	if (ret)
		return ret;
	if (isi->state != STATE_CAPTURE_READY)
		return -EIO;

	/*
	 * Do a codec request. Next SOF indicates start of capture,
	 * the one after that indicates end of capture.
	 */
	pr_debug("isi: starting capture\n");
	isi_writel(isi, CDBA, isi->capture_phys);

	spin_lock_irq(&isi->lock);
	isi->state = STATE_CAPTURE_WAIT_SOF;
	cr1 = isi_readl(isi, CR1);
	cr1 |= ISI_BIT(CODEC_ON);
	isi_writel(isi, CR1, cr1);
	isi_writel(isi, IER, ISI_CAPTURE_MASK);
	spin_unlock_irq(&isi->lock);

	return 0;
}

static void at91_isi_capture_done(struct at91_isi *isi,
				   int state)
{
	u32 cr1;

	cr1 = isi_readl(isi, CR1);
	cr1 &= ~ISI_BIT(CODEC_ON);
	isi_writel(isi, CR1, cr1);

	isi->state = state;
	wake_up_interruptible(&isi->capture_wq);
	isi_writel(isi, IDR, ISI_CAPTURE_MASK);
}

static irqreturn_t at91_isi_handle_streaming(struct at91_isi *isi, 
	int sequence){
	
	int reqnr;

	if(kfifo_get(isi->grabq, (unsigned char *) &reqnr,
			sizeof(int)) != sizeof(int)){
			
		/* as no new buffer is available we keep the
		* current one
		*/
		pr_debug("isi: dropping frame\n");
#ifdef ISI_CODEC
		isi_writel(isi, CDBA, 
			isi->current_buffer->fb_desc.fb_address);

		isi_writel(isi, CR1, ISI_BIT(CODEC_ON) | 
			isi_readl(isi, CR1));
#else
		/* TEST this has to be tested if it messes up the ISI
		 * streaming process */
		isi_writel(isi, PPFBD, (unsigned long) 
			&isi->video_buffer[isi->current_buffer->index]);
#endif
	}
	else{
		isi->current_buffer->status = FRAME_DONE;
		isi->current_buffer->sequence = sequence;
			
		do_gettimeofday(&isi->current_buffer->timestamp);
			
		/*isi->current_buffer->bytes_used = 
			ISI_VIDEO_MAX_FRAME_SIZE; */
			
		kfifo_put(isi->doneq, (unsigned char *) 
			&(isi->current_buffer->index), sizeof(int));
			
		isi->current_buffer = &(isi->video_buffer[reqnr]);
#ifdef ISI_CODEC
		isi_writel(isi, CDBA, 
			isi->current_buffer->fb_desc.fb_address);
		isi_writel(isi, CR1, ISI_BIT(CODEC_ON) | 
			isi_readl(isi, CR1));
#else
		/*TODO check if fbd corresponds to frame buffer */
#endif
		wake_up_interruptible(&isi->capture_wq);
	}
	return IRQ_HANDLED;
}

/* FIXME move code from ISR here
static irqreturn_t at91_isi_handle_capturing(struct at91_isi *isi){

}*/
/* isi interrupt service routine */
static irqreturn_t isi_interrupt(int irq, void *dev_id)
{
	struct at91_isi *isi = dev_id;
	u32 status, mask, pending;
	irqreturn_t ret = IRQ_NONE;
	/* TODO Should we set sequence to 0 upon each start sequence? */
	static int sequence = 0;

	spin_lock(&isi->lock);

	status = isi_readl(isi, SR);
	mask = isi_readl(isi, IMR);
	pending = status & mask;

	pr_debug("isi: interrupt status %x pending %x\n",
		 status, pending);
	if(isi->streaming){
		if(likely(pending & (ISI_BIT(FO_C_EMP) | ISI_BIT(FO_P_EMP)))){
		
			sequence++;
		 	ret = at91_isi_handle_streaming(isi, sequence);
		}
	}
	else{
	while (pending) {
		if (pending & (ISI_BIT(FO_C_OVF) | ISI_BIT(FR_OVR))) {
			at91_isi_capture_done(isi, STATE_CAPTURE_ERROR);
			pr_debug("%s: FIFO overrun (status=0x%x)\n",
				 isi->vdev.name, status);
		} else if (pending & ISI_BIT(SOF)) {
			switch (isi->state) {
			case STATE_IDLE:
				isi->state = STATE_CAPTURE_READY;
				wake_up_interruptible(&isi->capture_wq);
				break;
			case STATE_CAPTURE_READY:
				break;
			case STATE_CAPTURE_WAIT_SOF:
				isi->state = STATE_CAPTURE_IN_PROGRESS;
				break;
			/*
			case STATE_CAPTURE_IN_PROGRESS:
				at91_isi_capture_done(isi, STATE_CAPTURE_DONE);
				break;
				*/
			}
		}
		if (pending & ISI_BIT(FO_C_EMP)){
			if( isi->state == STATE_CAPTURE_IN_PROGRESS)
				at91_isi_capture_done(isi, STATE_CAPTURE_DONE);
		}

		if (pending & ISI_BIT(SOFTRST)) {
			complete(&isi->reset_complete);
			isi_writel(isi, IDR, ISI_BIT(SOFTRST));
		}

		status = isi_readl(isi, SR);
		mask = isi_readl(isi, IMR);
		pending = status & mask;
		ret = IRQ_HANDLED;
	}
	}
	spin_unlock(&isi->lock);

	return ret;
}

/* ------------------------------------------------------------------------
 *  IOCTL videoc handling
 *  ----------------------------------------------------------------------*/

/* --------Capture ioctls ------------------------------------------------*/
/* Device capabilities callback function.
 */
static int at91_isi_capture_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	strcpy(cap->driver, "at91-isi");
	strcpy(cap->card, "Atmel Image Sensor Interface");
	cap->version = AT91_ISI_VERSION;
	/* V4L2_CAP_VIDEO_CAPTURE -> This is a capture device
	 * V4L2_CAP_READWRITE -> read/write interface used
	 */
	cap->capabilities = (V4L2_CAP_VIDEO_CAPTURE 
			     | V4L2_CAP_READWRITE
			     );
	return 0;
}

/*  Input enumeration callback function. 
 *  Enumerates available input devices.
 *  This can be called many times from the V4L2-layer by 
 *  incrementing the index to get all avaliable input devices.
 */
static int at91_isi_capture_enum_input(struct file *file, void *priv,
				struct v4l2_input *input)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/* Just one input (ISI) is available */
	if (input->index != 0)
		return -EINVAL;

	/* Set input name as camera name */
	strlcpy(input->name, isi->camera->name, sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	
	/* Set to this value just because this should be set to a 
	 * defined value
	 */
	input->std = V4L2_STD_PAL;

	return 0;
}
/* Selects an input device.
 *  One input device (ISI) currently supported.
 */
static int at91_isi_capture_s_input(struct file *file, void *priv,
			     unsigned int index)
{
	if (index != 0)
		return -EINVAL;
	return 0;
}

/* Gets current input device.
 */
static int at91_isi_capture_g_input(struct file *file, void *priv,
			     unsigned int *index)
{
	*index = 0;
	return 0;
}

/* Format callback function 
 * Returns a v4l2_fmtdesc structure with according values to a
 * index.
 * This function is called from user space until it returns 
 * -EINVAL.
 */
static int at91_isi_capture_enum_fmt_cap(struct file *file, void *priv,
				  struct v4l2_fmtdesc *fmt)
{
	if (fmt->index != 0)
		return -EINVAL;

	/* if we want to pretend another ISI output 
	 * this is usefull if we input an other input format from a camera
	 * than specified in the ISI -> makes it possible to swap bytes 
	 * in the ISI output format but messes up the preview path output
	 */ 
	if(capture_v4l2_fmt){
		fmt->pixelformat = capture_v4l2_fmt;
	}
	else {
		/* This is the format the ISI tries to output */
		strcpy(fmt->description, "CrYCbY (VYUY) 4:2:2");
		fmt->pixelformat = V4L2_PIX_FMT_VYUY;
	}

	return 0;
}

static int at91_isi_capture_try_fmt_cap(struct file *file, void *priv,
			struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/* Just return the current format for now */
	memcpy(&vfmt->fmt.pix, &isi->format.pix,
		sizeof(struct v4l2_pix_format));

	return 0;
}

/* Gets current hardware configuration
 *  For capture devices the pixel format settings are 
 *  important.
 */
static int at91_isi_capture_g_fmt_cap(struct file *file, void *priv,
			       struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/* Return current pixel format */
	memcpy(&vfmt->fmt.pix, &isi->format.pix,
	       sizeof(struct v4l2_pix_format));

	return 0;
}

static int at91_isi_capture_s_fmt_cap(struct file *file, void *priv,
			       struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;
	int ret = 0;
	
	/* We have a fixed format so just copy the current format 
	 * back
	 */
	memcpy(&vfmt->fmt.pix, &isi->format.pix,
		sizeof(struct v4l2_pix_format));

	return ret;
}

/* ------------ Preview path ioctls ------------------------------*/
/* Device capabilities callback function.
 */
static int at91_isi_streaming_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	strcpy(cap->driver, "at91-isi");
	strcpy(cap->card, "Atmel Image Sensor Interface");
	cap->version = AT91_ISI_VERSION;
	/* V4L2_CAP_VIDEO_CAPTURE -> This is a capture device
	 * V4L2_CAP_READWRITE -> read/write interface used
	 * V4L2_CAP_STREAMING -> ioctl + mmap interface used
	 */
	cap->capabilities = (V4L2_CAP_VIDEO_CAPTURE 
			     | V4L2_CAP_READWRITE
			     | V4L2_CAP_STREAMING
			     );
	return 0;
}

/* Input enumeration callback function. 
 *  Enumerates available input devices.
 *  This can be called many times from the V4L2-layer by 
 *  incrementing the index to get all avaliable input devices.
 */
static int at91_isi_streaming_enum_input(struct file *file, void *priv,
				struct v4l2_input *input)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/* Just one input (ISI) is available */
	if (input->index != 0)
		return -EINVAL;

	/* Set input name as camera name */
	strlcpy(input->name, isi->camera->name, sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	
	/* Set to this value just because this should be set to a 
	 * defined value
	 */
	input->std = V4L2_STD_PAL;

	return 0;
}

/* Selects an input device.
 *  One input device (ISI) currently supported.
 */
static int at91_isi_streaming_s_input(struct file *file, void *priv,
			     unsigned int index)
{
	if (index != 0)
		return -EINVAL;

	return 0;
}

/* Gets current input device.
 */
static int at91_isi_streaming_g_input(struct file *file, void *priv,
			     unsigned int *index)
{
	*index = 0;
	return 0;
}

/* Format callback function 
 * Returns a v4l2_fmtdesc structure with according values to a
 * index.
 * This function is called from user space until it returns 
 * -EINVAL.
 */
static int at91_isi_streaming_enum_fmt_cap(struct file *file, void *priv,
				  struct v4l2_fmtdesc *fmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	if (fmt->index != 0)
		return -EINVAL;

	/* TODO: Return all possible formats 
	* This depends on ISI and camera.
	* A enum_fmt function or a data structure should be 
	* added to the camera driver.
	* For now just one format supported 
	*/
	if(streaming_v4l2_fmt){
		strcpy(fmt->description, "Pretended format");
	}
	else{
		strcpy(fmt->description, "Normal format");
	}
	/* The pretended and normal format are already set earlier */
	fmt->pixelformat = isi->streaming_format.pix.pixelformat;

	return 0;
}

static int at91_isi_streaming_try_fmt_cap(struct file *file, void *priv,
			struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/* FIXME For now we just return the current format*/
	memcpy(&vfmt->fmt.pix, &isi->streaming_format.pix,
		sizeof(struct v4l2_pix_format));
	return 0;
}

/* Gets current hardware configuration
 *  For capture devices the pixel format settings are 
 *  important.
 */
static int at91_isi_streaming_g_fmt_cap(struct file *file, void *priv,
			       struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;

	/*Copy current pixel format structure to user space*/
	memcpy(&vfmt->fmt.pix, &isi->streaming_format.pix,
	       sizeof(struct v4l2_pix_format));

	return 0;
}

static int at91_isi_streaming_s_fmt_cap(struct file *file, void *priv,
			       struct v4l2_format *vfmt)
{
	struct at91_isi_fh *fh = priv;
	struct at91_isi *isi = fh->isi;
	int ret = 0;
	
	/* Just return the current format as we do not support
	* format switching */
	memcpy(&vfmt->fmt.pix, &isi->streaming_format.pix,
		sizeof(struct v4l2_pix_format));

	return ret;
}

/* Checks if control is supported in driver
 * No controls currently supported yet
 */
/*
static int at91_isi_streaming_queryctrl(struct file *file, void *priv,
			   struct v4l2_queryctrl *qc)
{
	switch(qc->id){
	case V4L2_CID_BRIGHTNESS:
		strcpy(qc->name, "Brightness");
		qc->minimum = 0;
		qc->maximum = 100;
		qc->step = 1;
		qc->default_value = 50;
		qc->flags = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int at91_isi_streaming_g_ctrl(struct file *file, void *priv,
			struct v4l2_control *ctrl)
{
	switch(ctrl->id){
	case V4L2_CID_BRIGHTNESS:
		ctrl->value = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int at91_isi_streaming_s_ctrl(struct file *file, void *priv,
			struct v4l2_control *ctrl)
{
	switch(ctrl->id){
	case V4L2_CID_BRIGHTNESS:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
*/
static int at91_isi_reqbufs(struct file *file, void *private_data, 
			struct v4l2_requestbuffers *req)
{
	/* Only memory mapped buffers supported*/
	if(req->memory != V4L2_MEMORY_MMAP){
		pr_debug("AT91_ISI: buffer format not supported\n");
		return -EINVAL;
	}
	pr_debug("AT91_ISI: Requested %d buffers. Using %d buffers\n",
		req->count, video_buffers);
	/* buffer number is fixed for now as it is difficult to get 
	 * that memory at runtime */	
	req->count = video_buffers;
	memset(&req->reserved, 0, sizeof(req->reserved));
	return 0;
}

static int at91_isi_querybuf(struct file *file, void *private_data, 
			struct v4l2_buffer *buf)
{
	struct at91_isi_fh *fh = private_data;
	struct at91_isi *isi = fh->isi;
	struct frame_buffer *buffer; 

	if(unlikely(buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE))
		return -EINVAL;
	if(unlikely(buf->index >= video_buffers))
		return -EINVAL;

	buffer = &(isi->video_buffer[buf->index]);

	buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf->length = video_buffer_size;
	buf->memory = V4L2_MEMORY_MMAP;

	/* set index as mmap reference to the buffer */
	buf->m.offset = buf->index << PAGE_SHIFT;
	
	switch(buffer->status){
	case FRAME_UNUSED:
	case FRAME_ERROR:
	case FRAME_QUEUED:
		buf->flags |= V4L2_BUF_FLAG_QUEUED;
		buf->bytesused = buffer->bytes_used;
		break;
	case FRAME_DONE:
		buf->flags |= V4L2_BUF_FLAG_DONE;
		buf->bytesused = buffer->bytes_used;
		buf->sequence = buffer->sequence;
		buf->timestamp = buffer->timestamp;
		break;
	}

	buf->field = V4L2_FIELD_NONE; /* no interlacing stuff */

	if(buffer->mmap_count)
		buf->flags |= V4L2_BUF_FLAG_MAPPED;
	else 
		buf->flags &= ~V4L2_BUF_FLAG_MAPPED;

	pr_debug("at91_isi: querybuf index:%d offset:%d\n",
		buf->index, buf->m.offset);

	return 0;
}

static int at91_isi_qbuf(struct file *file, void *private_data,
			struct v4l2_buffer *buf)
{
	struct at91_isi_fh *fh = private_data;
	struct at91_isi *isi = fh->isi;
	struct frame_buffer *buffer;
	
	if(unlikely(buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE))
		return -EINVAL;
	if(unlikely(buf->index >= video_buffers || buf->index < 0)){
		pr_debug("Buffer index is not valid index=%d\n",buf->index);
		return -EINVAL;
	}
	if(unlikely(buf->memory != V4L2_MEMORY_MMAP)){
		pr_debug("Buffer is not of MEMORY_MMAP type\n"); 
		return -EINVAL;
	}

	buffer = &(isi->video_buffer[buf->index]);
	if(unlikely(buffer->status != FRAME_UNUSED)){
		pr_debug("Can't queue non unused frames\n");	
		return -EINVAL;
	}

	mutex_lock(&isi->mutex);
	buf->flags |= V4L2_BUF_FLAG_QUEUED;
	buf->flags &= ~V4L2_BUF_FLAG_DONE;
	buffer->status = FRAME_QUEUED;
	kfifo_put(isi->grabq, (unsigned char*) &buf->index, sizeof(int));
	mutex_unlock(&isi->mutex);

	return 0;
}

static int at91_isi_dqbuf(struct file *file, void *private_data,
			struct v4l2_buffer *buf)
{
	struct at91_isi_fh *fh = private_data;
	struct at91_isi *isi = fh->isi;
	struct frame_buffer *buffer;
	int reqnr = 0;

	if(unlikely(buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE))
		return -EINVAL;
	/* Mencoder does not set this flag
	 *
	if(unlikely(buf->memory != V4L2_MEMORY_MMAP)){
		pr_debug("isi: dequeue failed buffer not of mmapped type\n");
		return -EINVAL;
	}*/
	if((kfifo_len(isi->doneq) == 0) ){//&& (file->f_flags & O_NONBLOCK)){
		pr_debug("Done-queue is empty\n");
		return -EAGAIN;
	}
	/*
	if(wait_event_interruptible(isi->capture_wq, 
		kfifo_len(isi->doneq) != 0) < 0){
		pr_debug("Done-queue interrupted\n");
		return -EINTR;
	}
	*/
	if(!kfifo_get(isi->doneq, (unsigned char*) &reqnr, sizeof(int))){
		pr_debug("No new buffer ready\n");
		return -EBUSY;
	}
	buffer = &(isi->video_buffer[reqnr]);

	if(unlikely(buffer->status != FRAME_DONE)){
		if(isi->streaming == 0)
			return 0;
		pr_debug("isi: error, dequeued buffer not ready\n");
		return -EINVAL;
	}
	buf->index = reqnr;
	buf->bytesused = buffer->bytes_used;
	buf->timestamp = buffer->timestamp;
	buf->sequence = buffer->sequence;
	buf->m.offset = reqnr << PAGE_SHIFT;
	buffer->status = FRAME_UNUSED;
	buf->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_DONE;

	buf->length = isi->capture_buffer_size;
	buf->field = V4L2_FIELD_NONE;
	buf->memory = V4L2_MEMORY_MMAP;
	return 0;
}

static int at91_isi_streamon(struct file *file, void *private_data,
			enum v4l2_buf_type type)
{
	struct at91_isi_fh *fh = private_data;
	struct at91_isi *isi = fh->isi;
	int reqnr;
	struct frame_buffer *buffer;
	u32 cr1;

	if(unlikely(type != V4L2_BUF_TYPE_VIDEO_CAPTURE))
		return -EINVAL;
	
	if(!kfifo_get(isi->grabq, (unsigned char*) &reqnr, sizeof(int))){
		pr_debug("AT91_ISI: No buffer in IN-Queue, start of streaming\
			aborted (one buffer is required in IN-Queue)\n"); 
		return -EINVAL;
	}
	buffer = &(isi->video_buffer[reqnr]);
	

	spin_lock_irq(isi->lock);
	isi->streaming = 1;
	isi->current_buffer = buffer;
	cr1 = isi_readl(isi, CR1);
#ifdef ISI_CODEC
	isi_writel(isi, CDBA, buffer->fb_desc.fb_address);
	/* Enable codec path */
	cr1 |= ISI_BIT(CODEC_ON) | ISI_BIT(DIS);
#else
	isi_writel(isi, PPFBD, isi->fbd_list_start);
#endif
	/* Enable interrupts */
	isi_readl(isi, SR);
	/* FIXME enable codec/preview path according to setup */
	isi_writel(isi, IER, ISI_BIT(FO_C_EMP) | ISI_BIT(FO_P_EMP));

	cr1 |= ISI_BF(FRATE, frame_rate_scaler);

	/* Enable ISI module*/
	cr1 &= ~ISI_BIT(DIS);
	isi_writel(isi, CR1, cr1);
	spin_unlock_irq(isi->lock);
	pr_debug("AT91_ISI: Stream on\n");
	pr_debug("IMR 0x%x", isi_readl(isi, IMR));
	pr_debug("CR1 0x%x", isi_readl(isi, CR1));

	if(isi->camera)
		isi->camera->start_capture(isi->camera);

	return 0;
}

static int at91_isi_streamoff(struct file *file, void *private_data,
			enum v4l2_buf_type type)
{
	struct at91_isi_fh *fh = private_data;
	struct at91_isi *isi = fh->isi;
	int reqnr;

	if(unlikely(type != V4L2_BUF_TYPE_VIDEO_CAPTURE))
		return -EINVAL;

	spin_lock_irq(isi->lock);
	isi->streaming = 0;
#ifdef ISI_CODEC
	/* Disble codec path */
	isi_writel(isi, CR1, isi_readl(isi, CR1) & (~ISI_BIT(CODEC_ON)));
#endif
	/* Disable interrupts */
	isi_writel(isi, IDR, ISI_BIT(FO_C_EMP) | ISI_BIT(FO_P_EMP));
	
	/* Disable ISI module*/
	isi_writel(isi, CR1, isi_readl(isi, CR1) | ISI_BIT(DIS));
	spin_unlock_irq(isi->lock);
	
	if(isi->camera)
		isi->camera->stop_capture(isi->camera);

	while(kfifo_len(isi->grabq)){
		kfifo_get(isi->grabq, (unsigned char *) &reqnr, sizeof(int));
		kfifo_put(isi->doneq, (unsigned char *) &reqnr, sizeof(int)); 
	}
	for(reqnr = 0;  reqnr < video_buffers; reqnr++){
		isi->video_buffer[reqnr].status = FRAME_UNUSED;
	}
	pr_debug("AT91_ISI: Stream off\n");

	return 0;
}

/*----------------------------------------------------------------------------*/

static int at91_isi_init(struct at91_isi *isi){
	unsigned long timeout;

	/*
	 * Reset the controller and wait for completion. 
	 * The reset will only succeed if we have a 
	 * pixel clock from the camera.
	 */
	init_completion(&isi->reset_complete);
	isi_writel(isi, IER, ISI_BIT(SOFTRST));
	isi_writel(isi, CR1, ISI_BIT(RST));

	timeout = wait_for_completion_timeout(&isi->reset_complete,
		msecs_to_jiffies(100));

	if (timeout == 0) {
		return -ETIMEDOUT;
	}
	isi_writel(isi, IDR, ~0UL);
	
	at91_isi_set_default_format(isi);

	/* If no camera is active try to find one*/
	if (!isi->camera) {
		isi->camera = at91_isi_grab_camera(isi);
		
		/*If a camera was found and it offers a configuration
		 * interface we will use it.
		 */
		if (isi->camera && isi->camera->set_format)
		{
			at91_isi_set_camera_input(isi);
			isi->camera->set_format(isi->camera, &isi->format);
			
		}
		else
		{
			
		}
	}
	at91_isi_init_hardware(isi);
	
	return 0;
}

static int at91_isi_capture_close (struct file *file)
{
	struct at91_isi_fh *fh = file->private_data;
	struct at91_isi *isi = fh->isi;
	u32 cr1;

	mutex_lock(&isi->mutex);

	isi->capture_users--;
	kfree(fh);

	/* Stop camera and ISI  if driver has no users */
	if(!isi->stream_users) {
		if(isi->camera)
			isi->camera->stop_capture(isi->camera);

		spin_lock_irq(&isi->lock);
		cr1 = isi_readl(isi, CR1);
		cr1 |= ISI_BIT(DIS);
		isi_writel(isi, CR1, cr1);
		spin_unlock_irq(&isi->lock);
	}
	mutex_unlock(&isi->mutex);

	return 0;
}

static int at91_isi_capture_open (struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct at91_isi *isi = container_of(vdev, struct at91_isi, cdev);
	struct at91_isi_fh *fh;
	int ret = -EBUSY;

	pr_debug("%s: opened\n", vdev->name);

	mutex_lock(&isi->mutex);
	

	if (isi->capture_users) {
		pr_debug("%s: open(): device busy\n", vdev->name);
		goto out;
	}

	/* If the streaming interface has no users too we do a
	 * init of the hardware and software configuration.
	 */
	if(isi->stream_users == 0){
		ret = at91_isi_init(isi);
		if(ret)
			goto out;
	}
	
	ret = -ENOMEM;
	fh = kzalloc(sizeof(struct at91_isi_fh), GFP_KERNEL);
	if (!fh) {
		pr_debug("%s: open(): out of memory\n", vdev->name);
		goto out;
	}


	fh->isi = isi;
	file->private_data = fh;
	isi->capture_users++;

	ret = 0;

out:
	mutex_unlock(&isi->mutex);
	return ret;
}

static ssize_t at91_isi_capture_read(struct file *file, char __user *data,
			      size_t count, loff_t *ppos)
{
	struct at91_isi_fh *fh = file->private_data;
	struct at91_isi *isi = fh->isi;
	int state;
	int ret;

	state = STATE_IDLE;

	pr_debug("isi: read %zu bytes read_off=%u state=%u sizeimage=%u\n",
		count, fh->read_off, state, isi->format.pix.sizeimage);
	
	if(isi->camera)
		isi->camera->start_capture(isi->camera);
	
	pr_debug("after camera start capture\n");

	at91_isi_start_capture(isi);

	ret = wait_event_interruptible( isi->capture_wq,
			(isi->state == STATE_CAPTURE_DONE)
			|| (isi->state == STATE_CAPTURE_ERROR));
	if (ret)
		return ret;
	if (isi->state == STATE_CAPTURE_ERROR) {
		isi->state = STATE_IDLE;
		return -EIO;
	}

	fh->read_off = 0;

	count = min(count, (size_t)isi->format.pix.sizeimage - fh->read_off);
	pr_debug("after interrupt. data: %p isi->capture_buf: %p count: %d\n", data, isi->capture_buf, count); 

	ret = copy_to_user(data, isi->capture_buf + fh->read_off, count);
	if (ret)
		return -EFAULT;

	fh->read_off += count;
	if (fh->read_off >= isi->format.pix.sizeimage)
		isi->state = STATE_IDLE;

	return count;
}

static void at91_isi_capture_release (struct video_device *cdev)
{
	printk("isi release\n");	

}

/* ----------------- Streaming interface -------------------------------------*/
static void at91_isi_vm_open(struct vm_area_struct *vma){
	struct frame_buffer *buffer = 
		(struct frame_buffer *) vma->vm_private_data;
	buffer->mmap_count++;
	pr_debug("at91_isi: vm_open count=%d\n",buffer->mmap_count);
}

static void at91_isi_vm_close(struct vm_area_struct *vma){
	struct frame_buffer *buffer = 
		(struct frame_buffer *) vma->vm_private_data;
	pr_debug("at91_isi: vm_close count=%d\n",buffer->mmap_count);	
	buffer->mmap_count--;
	if(buffer->mmap_count < 0)
		printk(KERN_ALERT "at91_isi: mmap_count went negative\n");
}


static struct vm_operations_struct at91_isi_vm_ops = {
	.open = at91_isi_vm_open,
	.close = at91_isi_vm_close,
};

static int at91_isi_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long pfn;
	int ret;
	struct at91_isi_fh *fh = file->private_data;
	struct at91_isi * isi = fh->isi;
	struct frame_buffer *buffer = &(isi->video_buffer[vma->vm_pgoff]);
	unsigned long size = vma->vm_end - vma->vm_start;

	pr_debug("at91_isi: mmap called pgoff=%ld size=%ld \n", 
		vma->vm_pgoff, size);

	if(size > video_buffer_size){
		pr_debug("at91_isi: mmap requested buffer is to large\n");
		return -EINVAL;
	}
	if(vma->vm_pgoff > video_buffers){
		pr_debug("at91_isi: invalid mmap page offset\n");
		return -EINVAL;
	}
	pfn = isi->video_buffer[vma->vm_pgoff].fb_desc.fb_address >> PAGE_SHIFT;

	ret = remap_pfn_range(vma, vma->vm_start, pfn,
		vma->vm_end - vma->vm_start, vma->vm_page_prot);
	if(ret){
		return ret;
	}
	
	vma->vm_ops = &at91_isi_vm_ops;
	vma->vm_flags |= VM_DONTEXPAND; /* fixed size */
	vma->vm_flags |= VM_RESERVED;/* do not swap out */
	vma->vm_flags |= VM_DONTCOPY;
	vma->vm_flags |= VM_SHARED;
	vma->vm_private_data = (void *) buffer;
	at91_isi_vm_open(vma);

	pr_debug("at91_isi: vma start=0x%08lx, size=%ld phys=%ld \n",
		(unsigned long) vma->vm_start,
		(unsigned long) vma->vm_end - (unsigned long) vma->vm_start,
		pfn << PAGE_SHIFT);
	return 0;
}

static unsigned int at91_isi_poll(struct file *file, poll_table *wait)
{
	struct at91_isi_fh *fh = file->private_data;
	struct at91_isi *isi = fh->isi;
	unsigned int ret = 0;

	mutex_lock(&isi->mutex);
	poll_wait(file, &isi->capture_wq, wait);
	if(kfifo_len(isi->doneq))
		ret = POLLIN | POLLRDNORM;
	mutex_unlock(&isi->mutex);

	return ret;
}

static int at91_isi_stream_close (struct file *file)
{
	struct at91_isi_fh *fh = file->private_data;
	struct at91_isi *isi = fh->isi;
	u32 cr1;

	mutex_lock(&isi->mutex);

	isi->stream_users--;
	kfree(fh);

	/* Stop camera and ISI if driver has no users */
	if(!isi->capture_users) {
		if(isi->camera)
			isi->camera->stop_capture(isi->camera);

		spin_lock_irq(&isi->lock);
		cr1 = isi_readl(isi, CR1);
		cr1 |= ISI_BIT(DIS);
		isi_writel(isi, CR1, cr1);
		spin_unlock_irq(&isi->lock);
	}

	mutex_unlock(&isi->mutex);

	return 0;
}

static int at91_isi_stream_open (struct file *file)
{
	struct video_device *vdev = video_devdata(file);

	struct at91_isi *isi = to_at91_isi(vdev);

	struct at91_isi_fh *fh;
	int ret = -EBUSY;

	mutex_lock(&isi->mutex);
	
	// Just one user is allowed for the streaming device
	if (isi->stream_users) {
		pr_debug("%s: open(): device busy\n", vdev->name);
		goto out;
	}

	// If the capture interface is unused too we do a 
	// init of hardware/software configuration
	
	if(isi->capture_users == 0){
		ret = at91_isi_init(isi);
		if(ret)
			goto out;
	}

	ret = -ENOMEM;
	fh = kzalloc(sizeof(struct at91_isi_fh), GFP_KERNEL);
	if (!fh) {
		pr_debug("%s: open(): out of memory\n", vdev->name);
		goto out;
	}
	kfifo_reset(isi->grabq);
	kfifo_reset(isi->doneq);
	fh->isi = isi;
	file->private_data = fh;
	isi->stream_users++;

	ret = 0;

out:
	mutex_unlock(&isi->mutex);

	return ret;

	return 0;
}

static void at91_isi_stream_release (struct video_device *vdev)
{
	struct at91_isi *isi = to_at91_isi(vdev);

	pr_debug("%s: release\n", vdev->name);
	kfree(isi);

}

/* -----------------------------------------------------------------------*/

/* Streaming v4l2 device file operations */
// NOTE: This was originally file_operations, changed to v4l2_file_operations on 5/27/2010
static struct v4l2_file_operations at91_isi_streaming_fops = {
	.owner		= THIS_MODULE,
	.ioctl		= video_ioctl2,
	.open		= at91_isi_stream_open,
	.release	= at91_isi_stream_close,
	.mmap		= at91_isi_mmap,
	.poll		= at91_isi_poll,
};

/* Capture v4l2 device file operations */
// NOTE: This was originally file_operations, changed to compat_ioctl on 5/27/2010
static struct v4l2_file_operations at91_isi_capture_fops = {
	.owner		= THIS_MODULE,
	.open		= at91_isi_capture_open,
	.release	= at91_isi_capture_close,
	.read		= at91_isi_capture_read,
	.ioctl		= video_ioctl2,
};

static struct v4l2_ioctl_ops at91_isi_streaming_ioctl_ops = {

	.vidioc_querycap = at91_isi_streaming_querycap,
	.vidioc_enum_fmt_vid_cap = at91_isi_streaming_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap = at91_isi_streaming_try_fmt_cap,
	.vidioc_g_fmt_vid_cap = at91_isi_streaming_g_fmt_cap,
	.vidioc_s_fmt_vid_cap = at91_isi_streaming_s_fmt_cap,
	.vidioc_enum_input = at91_isi_streaming_enum_input,
	.vidioc_g_input = at91_isi_streaming_g_input,
	.vidioc_s_input = at91_isi_streaming_s_input,
/* No controls implemented yet
	isi->vdev.vidioc_queryctrl = at91_isi_streaming_queryctrl;
	isi->vdev.vidioc_g_ctrl = at91_isi_streaming_g_ctrl;
	isi->vdev.vidioc_s_ctrl = at91_isi_streaming_s_ctrl;
*/
	.vidioc_querybuf = at91_isi_querybuf,
	.vidioc_reqbufs = at91_isi_reqbufs,
	.vidioc_qbuf = at91_isi_qbuf,
	.vidioc_dqbuf = at91_isi_dqbuf,
	.vidioc_streamon = at91_isi_streamon,
	.vidioc_streamoff = at91_isi_streamoff,

};

static const struct v4l2_ioctl_ops at91_isi_capture_ioctl_ops = {
	.vidioc_querycap = at91_isi_capture_querycap,
	.vidioc_enum_fmt_vid_cap = at91_isi_capture_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap = at91_isi_capture_try_fmt_cap,
	.vidioc_g_fmt_vid_cap = at91_isi_capture_g_fmt_cap,
	.vidioc_s_fmt_vid_cap = at91_isi_capture_s_fmt_cap,
	.vidioc_enum_input = at91_isi_capture_enum_input,
	.vidioc_g_input = at91_isi_capture_g_input,
	.vidioc_s_input = at91_isi_capture_s_input,
};


static int __exit at91_isi_remove(struct platform_device *pdev)
{
	struct at91_isi *isi = platform_get_drvdata(pdev);
	int i;

	if (isi->camera)
		isi->camera->stop_capture(isi->camera);

	if (isi->camera)
		at91_isi_release_camera(isi, isi->camera);
	video_unregister_device(&isi->cdev);
	video_unregister_device(&isi->vdev);

	platform_set_drvdata(pdev, NULL);

	/* release capture buffer */
	//2010/06/29: if using ioremap, no need to dma_free
	//dma_free_coherent(&pdev->dev, capture_buffer_size,
	//isi->capture_buf, isi->capture_phys);

	/* release frame buffers */
	for(i = 0; i < video_buffers; i++){
		dma_free_coherent(&pdev->dev, 
			video_buffer_size,
			isi->video_buffer[i].frame_buffer,
			isi->video_buffer[i].fb_desc.fb_address);
	}

	kfifo_free(isi->doneq);
	kfifo_free(isi->grabq);
	
	free_irq(isi->irq, isi);
	iounmap(isi->regs);
	clk_disable(isi->pclk);
	//clk_put doesn't seem to do anything, ever
	clk_put(isi->pclk);

	/*
	 * Don't free isi here -- it will be taken care of by the
	 * release() callback.
	 */

	return 0;
}


static int __init at91_isi_probe(struct platform_device *pdev)
{
	unsigned int irq;
	struct at91_isi *isi;
	struct clk *pclk;//, *hclk;
	struct resource *regs;
	int ret;
	int i;
	int video_bytes_used = video_buffer_size;
	struct device *dev = &pdev->dev;
	struct isi_platform_data* pdata;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(!regs)
		return -ENXIO;
	printk("just before pclk\n");

	pclk = clk_get(&pdev->dev, "isi_clk");
	if (IS_ERR(pclk))
		return PTR_ERR(pclk);

	clk_enable(pclk);

	isi = kzalloc(sizeof(struct at91_isi), GFP_KERNEL);
	if(!isi){
		ret = -ENOMEM;
		dev_err(&pdev->dev, "can't allocate interface!\n");
		goto err_alloc_isi;
	}

	isi->pclk = pclk;

	spin_lock_init(&isi->lock);
	mutex_init(&isi->mutex);
	init_waitqueue_head(&isi->capture_wq);
	
	/* Initialize v4l2 capture device */
	isi->cdev.fops = &at91_isi_capture_fops;
	isi->cdev.ioctl_ops = &at91_isi_capture_ioctl_ops;
	strcpy(isi->cdev.name, "at91_isi_capture");
	isi->cdev.vfl_type = VFL_TYPE_GRABBER;
//	isi->cdev.type2 = VID_TYPE_CAPTURE;
	isi->cdev.minor = -1;
	isi->cdev.release =at91_isi_capture_release;
#ifdef DEBUG
	isi->cdev.debug = V4L2_DEBUG_IOCTL | V4L2_DEBUG_IOCTL_ARG;
#endif

	/* Initialize v4l2 streaming device */
	isi->vdev.fops = &at91_isi_streaming_fops;
	isi->vdev.ioctl_ops = &at91_isi_streaming_ioctl_ops;
	strcpy(isi->vdev.name, "at91-isi");
	isi->vdev.vfl_type = VFL_TYPE_GRABBER;
//	isi->vdev.type2 = VID_TYPE_CAPTURE;
	isi->vdev.minor = -1;
	isi->vdev.release = at91_isi_stream_release;
#ifdef DEBUG
	isi->vdev.debug = V4L2_DEBUG_IOCTL | V4L2_DEBUG_IOCTL_ARG;
#endif

	isi->regs = ioremap(regs->start, regs->end - regs->start + 1);
        if (!isi->regs) {
		ret = -ENOMEM;
		goto err_ioremap;
	}
	if(dev->platform_data){
		pdata = (struct isi_platform_data*) dev->platform_data;
		dev_info(&pdev->dev, "Reading configuration\n");
		image_hsize = pdata->image_hsize;
		image_vsize = pdata->image_vsize;
		if(pdata->prev_hsize)
			prev_hsize = pdata->prev_hsize;
		if(pdata->prev_vsize)
			prev_vsize = pdata->prev_vsize;
		gs_mode = pdata->gs_mode;
		input_format = pdata->pixfmt;
		frame_rate_scaler = pdata->frate;
		if(pdata->capture_v4l2_fmt)
			capture_v4l2_fmt = pdata->capture_v4l2_fmt;
		if(pdata->streaming_v4l2_fmt)
			streaming_v4l2_fmt = pdata->streaming_v4l2_fmt;
		if(pdata->cr1_flags & ISI_HSYNC_ACT_LOW)
			hsync_act_low = 1;
		if(pdata->cr1_flags & ISI_VSYNC_ACT_LOW)
			vsync_act_low = 1;
		if(pdata->cr1_flags & ISI_PXCLK_ACT_FALLING)
			pclk_act_falling = 1;
		if(pdata->cr1_flags & ISI_EMB_SYNC)
			has_emb_sync = 1;
		if(pdata->cr1_flags & ISI_CRC_SYNC)
			emb_crc_sync = 1;
		if(pdata->cr1_flags & ISI_FULL)
			isi_full_mode = 1;		
	}
	else{
		dev_info(&pdev->dev, "No config available using default values\n");
	}
	capture_buffer_size = image_hsize * image_vsize * 2;
	/* Only grayscale mode with gs_mode=1 uses 4 bytes for one
	 * pixel. Oll other modes use 2 bytes per pixel.*/
	if(gs_mode)
		video_buffer_size = prev_hsize * prev_vsize * 4;
	else
		video_buffer_size = prev_hsize * prev_vsize * 2;
	/* Round up buffer sizes to the next page if needed */
	video_buffer_size = PAGE_ALIGN(video_buffer_size);
	capture_buffer_size = PAGE_ALIGN(capture_buffer_size);

	isi_writel(isi, CR1, ISI_BIT(DIS));
	irq = platform_get_irq(pdev,0);
	ret = request_irq(irq, isi_interrupt, 0, "isi", isi);
	if (ret) {
		dev_err(&pdev->dev, "unable to request irq %d\n", irq);
		goto err_req_irq;
	}
	isi->irq = irq;
	/* Allocate ISI capture buffer */
	/* This would remap the top 8Mb of the RAM for use in DMA */
	isi->capture_buf = alloc_bootmem(0x800000);
//	isi->capture_buf = ioremap(0x3800000 /* 56Mb */, 0x800000 /* 8Mb */); //only returns virtual address
	if (!isi->capture_buf) 
	{
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate capture buffer\n");
		goto err_alloc_cbuf;
	}
	isi->capture_phys = __pa(isi->capture_buf);
	if (!isi->capture_phys) 
	{
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate capture buffer phys\n");
		goto err_alloc_cbuf;
	}

	/* Allocate and initialize video buffers */
	for(i=0;i < video_buffers; i++){
		memset(&isi->video_buffer[i], 0, sizeof(struct frame_buffer));
		isi->video_buffer[i].frame_buffer = 
			dma_alloc_coherent(&pdev->dev,
				video_buffer_size,
				(dma_addr_t *)
				&(isi->video_buffer[i].fb_desc.fb_address),
				GFP_KERNEL);
		if(!isi->video_buffer[i].frame_buffer){
			ret = -ENOMEM;
			dev_err(&pdev->dev, 
				"failed to allocate video buffer\n");
			goto err_alloc_vbuf;
		}
		
		isi->video_buffer[i].bytes_used = video_bytes_used;
		isi->video_buffer[i].status = FRAME_UNUSED;
		isi->video_buffer[i].index = i;

#ifdef DEBUG
	/* Put some color into the buffers */
	/*
		memset(isi->video_buffer[i].frame_buffer, (i*4)%0xFF, 
			video_buffer_size); 
			*/
#endif
	}
	/* set up frame buffer descriptor list for ISI module*/
	/* FIXME: The fbd list should be allocated with dma_alloc
	 * This is currently not the case.
	isi->fbd_list_start = dma_map_single(&pdev->dev,
		&isi->video_buffer[0].fb_desc,
		sizeof(struct fbd),
		DMA_NONE);
		*/
	isi->fbd_list_start = __pa(&isi->video_buffer[0].fb_desc);
	for(i=0; i < (video_buffers - 1); i++){
		isi->video_buffer[i].fb_desc.next_fbd_address =
		/*
			dma_map_single(&pdev->dev, 
				&isi->video_buffer[i+1].fb_desc,
				sizeof(struct fbd),
				DMA_NONE);*/
			__pa(&isi->video_buffer[i+1]);
	}
	/* FIXME
	 * isi->video_buffer[i].fb_desc.next_fbd_address =
	 * 	isi->fbd_list_start;
	 */
	isi->video_buffer[i].fb_desc.next_fbd_address = 
		__pa(&isi->video_buffer[0]);
	
	for(i=0;i < video_buffers; i++){
		dev_info(&pdev->dev,"video buffer: %d bytes at %p (phys %08lx)\n", 
		video_buffer_size, 
		isi->video_buffer[i].frame_buffer,
		(unsigned long) isi->video_buffer[i].fb_desc.fb_address);
	}
	dev_info(&pdev->dev,"capture buffer: %d bytes at %p (phys 0x%08x)\n",capture_buffer_size, isi->capture_buf,
		 isi->capture_phys);
	
	spin_lock_init(&isi->grabq_lock);
	isi->grabq = kfifo_alloc(sizeof(int) * video_buffers, GFP_KERNEL,
		&isi->grabq_lock);
	if(IS_ERR(isi->grabq)){
		dev_err(&pdev->dev, "fifo allocation failed\n");
		goto err_fifo_alloc1;
	}
	spin_lock_init(&isi->doneq_lock);
	isi->doneq = kfifo_alloc(sizeof(int) * video_buffers, GFP_KERNEL,
		&isi->doneq_lock);
	if(IS_ERR(isi->doneq)){
		dev_err(&pdev->dev, "fifo allocation failed\n");
		goto err_fifo_alloc2;
	}
	ret = video_register_device(&isi->cdev, VFL_TYPE_GRABBER, -1);
	if(ret){
		dev_err(&pdev->dev, "Registering capturing device failed\n");
		goto err_register1;
	}
	ret = video_register_device(&isi->vdev, VFL_TYPE_GRABBER, -1);
	if (ret){
		dev_err(&pdev->dev, "Registering streaming device failed\n");
		goto err_register2;
	}

	platform_set_drvdata(pdev, isi);

	dev_info(&pdev->dev, "Atmel ISI V4L2 device at 0x%08lx\n",
		 (unsigned long)regs->start);
	pr_debug("at91_isi module load\n");

	return 0;

err_register2:
	video_unregister_device(&isi->cdev);
err_register1:
	kfifo_free(isi->doneq);
err_fifo_alloc2:
	kfifo_free(isi->grabq);
err_fifo_alloc1:
err_alloc_vbuf:
	while(i--)
		dma_free_coherent(&pdev->dev, video_buffer_size,
				isi->video_buffer[i].frame_buffer,
				isi->video_buffer[i].fb_desc.fb_address);
err_alloc_cbuf:
        free_irq(isi->irq, isi);
err_req_irq:
	iounmap(isi->regs);
err_ioremap:
	kfree(isi);
err_alloc_isi:
	clk_disable(pclk);
//err_hclk:
	clk_put(pclk);

	return ret;

}


static struct platform_driver at91_isi_driver = {
	.probe		= at91_isi_probe,
	.remove		= __exit_p(at91_isi_remove),
	.driver		= {
        .name = "at91_isi",
	.owner = THIS_MODULE,
	},
};

static int __init at91_isi_init_module(void)
{
	printk("at91_isi_init_module running\n");
	return  platform_driver_probe(&at91_isi_driver, &at91_isi_probe);
}

static void __exit at91_isi_exit(void)
{
	platform_driver_unregister(&at91_isi_driver);
}


module_init(at91_isi_init_module);
module_exit(at91_isi_exit);

MODULE_AUTHOR("Lars Häring <lharing@atmel.com>");
MODULE_DESCRIPTION("The V4L2 driver for AVR32 Linux");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("video");