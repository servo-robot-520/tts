import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    default_config = os.path.join(
        get_package_share_directory("tts"), "config", "tts.yaml"
    )
    config = LaunchConfiguration("config")
    namespace = LaunchConfiguration("namespace")

    return LaunchDescription(
        [
            # 允许 duck_bringup 等上层 launch 传入自己的参数文件；
            # 不传时使用 tts 包内的默认配置。
            DeclareLaunchArgument(
                "config",
                default_value=default_config,
                description="Path to the tts parameter file",
            ),
            # 默认不设置 namespace，节点全名为 /audio_tts；
            # tts_interface 的默认 endpoint 也以 /audio_tts 为前缀。
            DeclareLaunchArgument(
                "namespace",
                default_value="",
                description="Optional ROS namespace; empty means no namespace",
            ),
            Node(
                package="tts",
                executable="tts_node",
                name="audio_tts",
                namespace=namespace,
                output="screen",
                parameters=[config],
            ),
        ]
    )
