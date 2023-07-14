import torch
import tt_lib

from tt_lib.fallback_ops import fallback_ops
from python_api_testing.models.EfficientNet.tt.efficientnet_conv import (
    TtEfficientnetConv2d,
)


class TtEfficientnetSqueezeExcitation(torch.nn.Module):
    """
    This block implements the Squeeze-and-Excitation block from https://arxiv.org/abs/1709.01507 (see Fig. 1).
    Parameters ``activation``, and ``scale_activation`` correspond to ``delta`` and ``sigma`` in eq. 3.

    Args:
        input_channels (int): Number of channels in the input image
        squeeze_channels (int): Number of squeeze channels
        activation (Callable[..., torch.nn.Module], optional): ``delta`` activation. Default: ``torch.nn.SiLU``
        scale_activation (Callable[..., torch.nn.Module]): ``sigma`` activation. Default: ``torch.nn.Sigmoid``
    """

    def __init__(
        self,
        state_dict,
        base_address,
        device,
        input_channels: int,
        squeeze_channels: int,
    ):
        super().__init__()

        self.avgpool = fallback_ops.AdaptiveAvgPool2d(1)

        self.fc1 = TtEfficientnetConv2d(
            state_dict=state_dict,
            base_address=f"{base_address}.fc1",
            device=device,
            in_channels=input_channels,
            out_channels=squeeze_channels,
            kernel_size=1,
        )

        self.fc2 = TtEfficientnetConv2d(
            state_dict=state_dict,
            base_address=f"{base_address}.fc2",
            device=device,
            in_channels=squeeze_channels,
            out_channels=input_channels,
            kernel_size=1,
        )

        self.activation = fallback_ops.silu
        self.scale_activation = tt_lib.tensor.sigmoid

    def _scale(self, x):
        scale = self.avgpool(x)
        scale = self.fc1(scale)
        scale = self.activation(scale)
        scale = self.fc2(scale)
        return self.scale_activation(scale)

    def forward(self, x):
        scale = self._scale(x)

        x = tt_lib.tensor.bcast(
            x, scale, tt_lib.tensor.BcastOpMath.MUL, tt_lib.tensor.BcastOpDim.HW
        )
        return x
