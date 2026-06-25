from setuptools import setup, find_packages
import os
import subprocess

setup(
    name='capdb',
    version='0.1.0',
    description='CapDB network client library for Python',
    author='CapDB Contributors',
    license='MIT',
    packages=find_packages(),
    python_requires='>=3.8',
    install_requires=[],
    extras_require={
        'dev': ['pytest>=7.0', 'pytest-asyncio>=0.20.0', 'black>=23.0'],
        'async': ['asyncio-contextmanager>=1.0'],
    },
    classifiers=[
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: MIT License',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: Python :: 3.11',
        'Programming Language :: Python :: 3.12',
        'Operating System :: OS Independent',
        'Topic :: Database',
    ],
    project_urls={
        'Repository': 'https://github.com/rickcollette/CapDB',
        'Documentation': 'https://github.com/rickcollette/CapDB/tree/main/bindings/python',
    },
)
