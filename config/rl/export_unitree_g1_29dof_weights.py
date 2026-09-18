"""Export the Unitree G1 29-DOF velocity policy (ONNX MLP) for the dense, ONNX-free runtime.

The published policy is a plain MLP actor whose ONNX graph is Gemm(transB=1)+Elu, so the weights can
be copied straight out of the initializers into the flat float32 layout `DensePolicy` expects. No
torch is required.

Source:
    unitree_rl_lab/deploy/robots/g1_29dof/config/policy/velocity/v0/exported/policy.onnx
    unitree_rl_lab/deploy/robots/g1_29dof/config/policy/velocity/v0/params/deploy.yaml

Usage:
    python export_unitree_g1_29dof_weights.py <policy.onnx> <out.bin>

Weight file layout (little-endian float32, no header), matching DensePolicy::Kind::Mlp:
    for each layer: weight [out x in], bias [out]

Requires: onnx, numpy (onnxruntime only for the optional cross-check).
"""
import sys

import numpy as np
import onnx
from onnx import numpy_helper

# Gemm nodes reference these in graph order; each weight is stored [out, in] with transB=1.
WEIGHT_ORDER = [
    "actor.0.weight", "actor.0.bias",
    "actor.2.weight", "actor.2.bias",
    "actor.4.weight", "actor.4.bias",
    "actor.6.weight", "actor.6.bias",
]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    onnx_path, out_path = sys.argv[1], sys.argv[2]

    model = onnx.load(onnx_path)
    initializers = {
        tensor.name: numpy_helper.to_array(tensor).astype(np.float32)
        for tensor in model.graph.initializer
    }
    for name in WEIGHT_ORDER:
        if name not in initializers:
            print(f"missing initializer: {name}")
            return 1

    blob = np.concatenate([initializers[name].reshape(-1) for name in WEIGHT_ORDER])
    blob.astype("<f4").tofile(out_path)
    print("wrote", out_path, blob.size, "floats")
    for name in WEIGHT_ORDER:
        print(" ", name, initializers[name].shape)

    # Optional cross-check against onnxruntime, so a wrong tensor order cannot slip through.
    try:
        import onnxruntime as ort
    except ImportError:
        return 0

    in_shape = [d.dim_value for d in model.graph.input[0].type.tensor_type.shape.dim]
    out_shape = [d.dim_value for d in model.graph.output[0].type.tensor_type.shape.dim]
    obs = (np.random.randn(*in_shape) * 0.3).astype(np.float32)

    activation = obs
    for layer in range(0, len(WEIGHT_ORDER), 2):
        weight = initializers[WEIGHT_ORDER[layer]]
        bias = initializers[WEIGHT_ORDER[layer + 1]]
        activation = activation @ weight.T + bias
        if layer + 2 < len(WEIGHT_ORDER):
            activation = np.where(activation > 0, activation, np.expm1(activation))

    session = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    reference = session.run(None, {session.get_inputs()[0].name: obs})[0]
    print("obs", in_shape, "action", out_shape)
    print("max abs diff vs onnxruntime:", float(np.abs(activation - reference).max()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
