"""Clock-consistency regression for image flow plus pose correction.

The measured image displacement contains head and object motion.  Reusing the
same fraction for optical-flow warp and submitted-pose correction cancels the
head component; object motion remains deliberately predicted by image flow.
"""
import numpy as np

def evaluate(t_image, t_pose, delta_pose=np.array([3., -2.]), obj=np.array([1.25, .5])):
    # In image coordinates, head motion is -delta_pose; object motion is obj.
    flow=-delta_pose+obj
    # Pose reprojection contributes +delta_pose, cancelling the image-space
    # head term when both paths use the same fraction.
    warped=t_image*flow
    pose=t_pose*delta_pose
    head_error=(t_pose-t_image)*delta_pose
    object_advance=t_image*obj
    total=warped+pose
    return head_error, object_advance, total

def main():
    h,o,total=evaluate(2/3,2/3)
    print('equal clocks: head_error=',h,'object_advance=',o,'residual=',total)
    assert np.allclose(h,0) and np.allclose(total,o)
    h2,o2,total2=evaluate(2/3,1/3)
    print('unequal clocks: head_error=',h2,'object_advance=',o2,'residual=',total2)
    assert np.linalg.norm(h2)>1e-6 and not np.allclose(total2,o2)
    print('PASS: equal fractions cancel head motion; object advance remains; unequal fractions leave head error')

if __name__=='__main__': main()
