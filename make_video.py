import cv2
import subprocess
import tempfile as tf
import pandas   as pd
import numpy    as np
import os

def read_raw(file_name, points):
    raw = np.fromfile(file_name, dtype=np.uint8)
    real_points = [points[0], points[1], 4]
    raw = np.squeeze(raw.reshape(real_points))
    img = raw[:,:,(1, 2, 3, 0)]
    return img

def get_files(directory):
    files = []
    for filename in os.listdir(directory):
        base, ext = os.path.splitext(filename)
        if ext == '.bin':
            files.append(filename)
    return files

def make_frames(directory, points):
    imgs = []
    for f in get_files(directory):
        raw_file_name = os.path.join(directory, f)
        raw           = read_raw(raw_file_name, points)
        imgs.append(raw)
    return imgs


def make_video(name, frames, framerate):
    with tf.TemporaryDirectory() as tmp:
        count = 0
        for frame in frames:
            cv2.imwrite(os.path.join(tmp, "frame_{:02d}.png".format(count)), frame)
            count = count + 1

        ffmpeg_command = [
            "ffmpeg",
            "-y",
            "-framerate", "{:d}".format(framerate),
            "-i",         os.path.join(tmp, 'frame_%02d.png'),
            "-c:v",       "libx265",
            "-crf",       "18",
            name
        ]
        subprocess.run(ffmpeg_command)


##################################
save = True
out_dir   = "/tmp/downloads"
points    = [1080, 1920]
framerate = 60
if save:
    os.makedirs(out_dir, mode=0o644, exist_ok=True)

frames = make_frames("/tmp/downloads/out", points)
if save:
    make_video(os.path.join(out_dir, "volumes.mp4"), frames, framerate)

#cv2.imshow("", frames[0])
#cv2.waitKey(0)
