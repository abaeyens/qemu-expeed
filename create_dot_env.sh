#!/bin/bash
{
  echo "USER_ID=$(id -u)"
  echo "GROUP_ID=$(id -g)"
} > .env
